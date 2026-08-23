module;

//DirectXTk12のSimpleMathとBullet3が必要です
//https://github.com/microsoft/DirectXTK12
//
//↑のつかいかた
//①https://github.com/microsoft/DirectXTK12/tree/main/SrcのSimpleMath.cppとSimpleMath.hをダウンロード
//②SimpleMath.hをインクルードファイルパスの通っている所に置く
//③ソリューションエクスプローラでSimpleMath.cppをプロジェクトのソースファイルに追加する
//
// Bullet3
//https://github.com/bulletphysics/bullet3
//↑のつかいかた
//①git cloneで適当なフォルダ(ここではx:\bullet3とします)にclone
//②ソリューションエクスプローラでx:\bullet3\src\*.cpp, *.hをプロジェクトのソースファイルに追加する
//③プロジェクト -> プロジェクトのプロパティ -> C/C++ -> 全般 -> 追加のインクルードディレクトリ に x:\bullet3\src を追加
//④プロジェクト -> プロジェクトのプロパティ -> C/C++ -> プリプロサッサ で BT_USE_SSE_IN_API マクロを追加
//
// それから、Bullet3.25のバグ修正(noname0310氏に感謝します)
// src\BulletDynamics\ConstraintSolver\btGeneric6DofConstraint.cpp の746行目
// 
//		btVector3 c = m_calculatedTransformB.getOrigin() - transA.getOrigin();
//		↓
//		btVector3 c = m_calculatedTransformA.getOrigin() - transA.getOrigin();
//		と、修正してください
// 
// 
// 
//文字列は当面std::wstringで扱う
//std::formatがUTF-8対応になってstd::u8stringが実用的になったらまた考えるかもしれない

//STL
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

//DirectX
#include <DirectXMath.h>

//外部ライブラリ
#include <btBulletCollisionCommon.h>
#include <btBulletDynamicsCommon.h>
#include <SimpleMath.h>


// PMXLoader 
//
// 
// 参考文献
//
// PMXファイルフォーマット
// 極北P氏 PMXeditor付属 PMX仕様.txt
//
// VMDファイルフォーマット
// 針金P氏 "VMDメモ"
// https://hariganep.seesaa.net/article/201103article_1.html
// 
// Rodolphe Vaillant氏 "Cyclic Coordonate Descent Inverse Kynematic (CCD IK)"
// http://rodolphe-vaillant.fr/entry/114/cyclic-coordonate-descent-inverse-kynematic-ccd-ik 
//
// Johnathon Selstad氏 "Inverse Kinematics"
// https://zalo.github.io/blog/inverse-kinematics/
//
// benikabocha氏 "saba"
// https://github.com/benikabocha/saba
//
// Andante氏 "IbukiHash"
// https://gist.github.com/andanteyk/7581e025cc7e9af796b10032c61015a8


//TODO :
// 物理後変形ボーン
// ローカル付与

/*MikuMikuDayo β2以降の更新履歴

2026-0303
・循環参照を含む付与構造を持ったモデルを読み込むと永久ループするバグを修正

2026-0222
・PoseSolver::Join()においてIKボーン名をコピーし忘れていたのを修正

2025-1214
・SerializedVMDCamera.parthの代入が反転していたのを修正

2025-1027
・SerializedVMDCamera.rotationの代入が正しく行われていなかったのを修正

2025-0818
・IKを本家MMDの挙動に近づけた。PMX::MathにSmoothStep()など追加

2025-0802
・多段付与が正しく行われていなかったのを修正

2025-0726
・VMD::VMD()引数にtarget追加。これにより、ANSIに変換して先頭15バイトがマッチしていればボーン・モーフ用のキーとして受け入れられるようになった
・PMXModel::ResolveDuplicatedBoneNames(), PMXModel::ResolveDuplicatedMaterialNames()のロジック修正

2025-0425
・PMXModel::ResolveDuplicatedBoneNames(), PMXModel::ResolveDuplicatedMaterialNames()追加
 
2025-0220
・PMX2.1モデルも読み込む事は可能にした(仕様の実装は順次行う予定)
・インパルスモーフ対応
 
*/


export module PMXLoader;


export namespace PMX {
	using namespace DirectX;

	constexpr int BulletPhysicsVersion = BT_BULLET_VERSION;

	class PMXException : public std::runtime_error
	{
	private:
		std::string message;
	public:
		PMXException(const std::string& msg) : runtime_error(msg) {};
	};

	struct PMXVertex {
		XMFLOAT3 position = {};
		XMFLOAT3 normal = {};
		XMFLOAT2 uv = {};
		XMFLOAT4 extra_uv[4] = {};
		int weightType = 0;	//ボーン変形方式 0:BDEF, 1:BDEF2, 2:BDEF4, 3:SDEF, 4:QDEF(PMX2.1)
		int bone[4] = {};	//ボーンインデックス
		float weight[4] = {};//ボーンウェイト
		XMFLOAT3 sdef_c = {};
		XMFLOAT3 sdef_r0 = {};
		XMFLOAT3 sdef_r1 = {};
		float edge = 1.0;	//エッジ倍率
	};

	struct PMXMaterial {
		std::wstring name;
		std::wstring name_e;
		XMFLOAT4 diffuse = {};
		XMFLOAT3 specular = {};
		float shininess = 0;
		XMFLOAT3 ambient = {};
		//描画ﾌﾗｸﾞ 0x01:両面, 0x02:地面影, 0x04:セルフシャドウキャスターになる, 0x08:セルフシャドウ描画, 0x10:エッジ描画
		//PMX2.1以降 0x20:頂点カラー, 0x40:Point描画 0x80:Line描画
		int drawFlag = 0;	
		XMFLOAT4 edgeColor = {};
		float edgeSize = 0;	//エッジサイズ、PMX2.1ではPoint描画時のポイントサイズとしても解釈される
		int tex = 0;	//テクスチャ番号
		int spTex = 0;
		int spmode = 0;	//スフィアモード 0:無効 1:乗算, 2:加算 3:サブテクスチャ
		int toonFlag = 0;	//共有Toonフラグ 0:継続値は個別Toon 1:継続値は共有Toon
		int toonTex = 0;	//テクスチャインデックス、または共有toonテクスチャ番号(0～9がtoon01.bmp～toon10.bmpに対応)
		int vertexCount = 0;	//頂点数(必ず3の倍数)
	};

	struct PMXIKLink {
		int bone = 0;
		int isLimit = 0;	//角度制限(bool)
		XMFLOAT3 low = {}, hi = {};	//角度制限の下限/上限
	};

	struct PMXBone {
	public:
		bool m_isIKLink = false;
		std::wstring name;
		std::wstring name_e;
		XMFLOAT3 position = {};	//回転中心
		int parent = -1;//親ボーン番号
		int level = 0;	//変形階層
		int flag = 0;	//ボーンフラグ(PMX仕様参照のこと)
		XMFLOAT3 toOffset = {};	//座標オフセット
		int toBone = -1;	//接続先ボーン
		int appendParent = -1;	//付与親ボーン
		float appendRatio = 1.0f;	//付与率
		XMFLOAT3 fixAxis = {};	//固定軸
		XMFLOAT3 localAxisX = {}, localAxisZ = {};	//ローカル軸X,Z
		int	externalKey = -1;//外部親キー
		int IKTarget = -1;	//IKターゲット
		int IKLoopCount = 0;//IKループ回数
		float IKAngle = 0;	//IKループ計算時の1回あたりの制限角度
		int IKLinkCount = 0;//IKリンク数
		
		bool IsPhysics = false;//PMXデータには入ってない情報。物理ボーンかどうか？剛体の関連ボーンかつ剛体がボーン追従でない場合はtrue
		int physicsMode = 0;	//同上、モデルから読み出した状態で判明する物理モード

		std::vector<PMXIKLink>IKLinks;
		bool IsIKLink() const { return m_isIKLink; }			//IKリンクボーンか？(PMX仕様にあるフラグではないが有用なので)
		bool IsIK() const { return flag & 0x0020; }				//IKのゴール
		bool IsAppendRotation() const { return flag & 0x0100; }	//回転付与される
		bool IsAppendTranslation() const { return flag & 0x0200; }//移動付与される
		bool IsAfterPhysics() const { return flag & 0x1000; }		//物理後変形
		bool IsExternal() const { return flag & 0x2000; }			//外部親変形

		//PMXの仕様にはあるが本家MMDが対応していない属性
		bool IsLocalAppend() const { return flag & 0x0080; }		//ローカル付与

		//PMXe、MMDなどでの編集の時のみ使われる属性
		bool IsLocalFrame() const { return flag & 0x0800; }		//ローカル軸
		bool IsFixAxis() const { return flag & 0x0400; }			//軸固定
		bool IsRotation() const { return flag & 0x0002; }			//各種操作を行う事が出来るか？
		bool IsTranslation() const { return flag & 0x0004; }
		bool IsVisible() const { return flag & 0x0008; }
		bool IsControllable() const { return flag & 0x0010; }
	};

	//頂点モーフのオフセット
	struct PMXVertexMorphOffset {
		int vertex = 0;
		XMFLOAT3 offset = {};
	};

	//UVモーフのオフセット
	struct PMXUVMorphOffset {
		int vertex = 0;
		int iuv = 0;	//どのUVに対するオフセットか？0が基本UV、1～4で追加UV1～4
		XMFLOAT4 offset = {};
	};

	//ボーンモーフのオフセット
	struct PMXBoneMorphOffset {
		int bone = 0;
		XMFLOAT3 translation = {};
		XMFLOAT4 rotation = {};
	};

	//材質モーフのオフセット
	struct PMXMaterialMorphOffset {
		int material = 0;	//対象材質。-1の時は「全材質対象」
		int op = 0;	//演算方式 0:乗算 1:加算
		XMFLOAT4 diffuse = {};
		XMFLOAT3 specular = {};
		float shininess = 0;
		XMFLOAT3 ambient = {};
		XMFLOAT4 edgeColor = {};
		float edgeSize = 0;
		XMFLOAT4 tex = {};
		XMFLOAT4 spTex = {};
		XMFLOAT4 toon = {};
	};

	//グループモーフのオフセット
	struct PMXGroupMorphOffset {
		int morph = 0;
		float ratio = 0;
	};

	//フリップモーフのオフセット
	struct PMXFlipMorphOffset {
		int morph = 0;
		float value = 0;
	};

	//インパルスモーフのオフセット
	struct PMXImpulseMorphOffset {
		int body = 0;		//剛体番号
		bool isLocal = false;	//ローカル軸？
		XMFLOAT3 linear;	//平行移動分
		XMFLOAT3 angular;	//回転分
	};


	struct PMXMorph {
		std::wstring name;
		std::wstring name_e;
		int panel = 0;			//操作パネルのどこに置かれるか？ 0:システム予約 1:眉(左下) 2: 2:目(左上) 3:口(右上) 4:その他(右下)
		int kind = 0;			//モーフの種類 0:グループ, 1:頂点, 2:ボーン, 3:UV, 4:追加UV1, 5:追加UV2, 6:追加UV3, 7:追加UV4, 8:材質 9:フリップ 10:インパルス
		int offsetCount = 0;	//オフセットの数
		std::vector<::byte>offsets;	//オフセットデータ(型が不定なので連続したメモリ領域を確保するためにbyte型のvectorとする)
	};


	struct PMXNodeItem {
		bool isBone = false;	//この表示枠はボーン用
		bool isMorph = false;	//この表示枠はモーフ用
		int index = 0;		//この表示枠に割り当てられたボーン/モーフ番号
	};

	//表示枠
	struct PMXNode {
		std::wstring name;		//枠名
		std::wstring name_e;	//枠名(英語)
		int flag = 0;	//特殊枠フラグ 0:通常枠 1:特殊枠
		std::vector<PMXNodeItem> items;
	};

	//剛体
	struct PMXBody {
		std::wstring name;
		std::wstring name_e;
		int bone = -1;
		int group = 0;			//自分の所属している衝突グループ(0-15)
		unsigned short passGroup = 0;	//非衝突グループフラグ(ビットフラグ)
		int boxKind = 0;		//形状 0:球, 1:箱, 2:カプセル
		XMFLOAT3 boxSize = {};
		XMFLOAT3 position = {};
		XMFLOAT3 rotation = {};
		float mass = 1;                     // 質量
		float positionDamping = 0;          // 移動減衰
		float rotationDamping = 0;          // 回転減衰
		float restitution = 0;              // 反発力
		float friction = 0;                 // 摩擦力
		int mode = 0;						//剛体のモード 0:ボーン追従(static), 1:物理演算(dynamic), 2:ボーン追従+位置合わせ
		bool isPass(int g) const { return passGroup & (1U << g); }	//グループ番号gは被衝突グループに含まれるか？
	};

	//ジョイント
	struct PMXJoint {
		std::wstring name;
		std::wstring name_e;
		int bodyA = 0;
		int bodyB = 0;

		int kind = 0;      // Joint種類 | 0:Sp6DOF, 1:G6DOF, 2:P2P, 3:ConeTwist, 4:Slider, 5:Hinge (PMX2.0では0のみ対応)

		XMFLOAT3 position = {};   // 位置
		XMFLOAT3 rotation = {};   // 回転(ラジアン角)

		XMFLOAT3 moveLo = {};      // 移動下限 ※Sp6DOFの場合(同以下)
		XMFLOAT3 moveHi = {};     // 移動上限
		XMFLOAT3 angleLo = {};		//回転下限
		XMFLOAT3 angleHi = {};		//回転上限
		XMFLOAT3 springMove = {};	//バネ定数-移動
		XMFLOAT3 springRotate = {};	//バネ定数-回転
	};

	//文字列のエンコード(読み込み時に変換するための物。読み込んだ後は全てwchar(utf16)に変換される)
	enum class PMXEncoding { utf8, utf16 };


	/*** GPU用ヘルパー ***/
	//VertexShaderやComputeShaderでスキニング、頂点モーフの処理をするためのテーブル作成用
	
	//GPUスキニング処理用
	struct GPUSkinning {
		int iBone[4];		//ボーン番号
		float weight[4];	//各ボーンのウェイト(SDEFの場合はweight[1-3]に(sdef_r0-sdef_r1)/2.xyzを入れる)
		int weightType;		//変形方式(0-2:BDEF1,2,4 / 3:SDEF)
		XMFLOAT3 sdef_c;	//SDEFパラメータ
	};

	//GPU頂点・UVモーフ処理用、これの配列をモーフテーブルと呼ぶ
	struct GPUMorphItem {
		int iMorph = 0;		//対応するモーフ番号(PoseSolver::morphValues[iMorph]の値を見て、頂点の位置またはUVをどれだけ動かすか決める)
		int target = 0;		//-1:位置, 0:UV, 1～4:追加UV1～4
		DirectX::XMFLOAT4 delta = DirectX::XMFLOAT4(0, 0, 0, 0);		//モーフ値1につき座標orUVはどれだけ動くか
	};

	//各頂点がモーフテーブルのどこに幾つモーフを持っているか
	struct GPUMorphPointer {
		int where;	//テーブルのどこから自分に割り当てられたモーフ情報が始まるか？ -1で割り当て無し
		int count;	//いくつアイテムがあるか
	};


	class PMXModel {
	private:
		//ファイルに格納されている時のエンコード
		PMXEncoding m_encoding = PMXEncoding::utf16;	//※文字列は全部読み取り時にwchar(utf16)に変換される
		//ファイルからの読み取り時にしか使われない、各インデックスのファイル内でのサイズ
		int m_size_vi = 1;
		int m_size_ti = 1;
		int m_size_mi = 1;
		int m_size_bi = 1;
		int m_size_moi = 1;
		int m_size_ri = 1;
		//ファイル読み取り用メソッド
		PMXVertex FReadVertex(FILE* fp);
		PMXMaterial FReadMaterial(FILE* fp);
		PMXBone FReadBone(FILE* fp);
		PMXMorph FReadMorph(FILE* fp);
		PMXNodeItem FReadNodeItem(FILE* fp);
		PMXNode FReadNode(FILE* fp);
		PMXBody FReadBody(FILE* fp);
		PMXJoint FReadJoint(FILE* fp);
		std::wstring FReadString(FILE* fp) const;
		int m_extra_uv = 0;
	public:
		float version = 0;	//PMXモデルのバージョン
		int extraUV() const { return m_extra_uv; }	//有効な追加UVの数
		std::vector<PMXVertex> vertices = {};	//頂点
		std::vector<int>indices = {};			//頂点インデクス
		std::vector<std::wstring>textures = {};		//テクスチャファイル名(PMXファイルに格納されている状態まま)
		std::vector<std::wstring>absTextures = {};	//テクスチャファイル名(絶対パスとして正規化済み)
		std::vector<PMXMaterial>materials = {};//材質
		std::vector<PMXBone>bones = {};		//ボーン
		std::vector<PMXMorph>morphs = {};	//モーフ
		std::vector<PMXNode>nodes = {};		//表示枠
		std::vector<PMXBody>bodies = {};//剛体
		std::vector<PMXJoint>joints;	//ジョイント
		std::wstring name = {};		//モデル名
		std::wstring name_e = {};	//モデル名(英語)
		std::wstring description = {};	//モデル説明文
		std::wstring description_e = {};	//モデル説明文(英語)
		//PMXファイルfnameを読み込む
		//fname:PMXファイルのフルパス, headerOnlyがtrueの時、データ全体は読み込まず、モデル名や説明文のみ読み込む
		PMXModel(const wchar_t* fname, bool headerOnly = false);
		PMXModel() {};	//空のPMXモデルを作る

		//GPUで扱う場合のヘルパー
		std::vector<GPUSkinning> skinTable = {};			//GPUスキニング用情報
		std::vector<GPUMorphItem> morphTable = {};			//GPU頂点・UVモーフ処理用のテーブル
		std::vector<GPUMorphPointer> morphPointers = {};	//各頂点がモーフテーブルのどこに何個のモーフ情報を持っているか？
		std::vector<int> face2material = {};				//面番号→材質番号テーブル

		//MMDayo向けヘルパー
		//重複したボーン名を持ったボーンがある場合、2番目以降のボーン名の末尾に0、1、2…と付ける
		//返り値は名前の変更をされたボーンの番号のset
		std::set<int> ResolveDuplicatedBoneNames();
		//重複した材質名を持った材質がある場合、2番目以降の材質名の末尾に0、1、2…と名前と付ける
		//返り値は名前の変更をされた材質番号のset
		std::set<int> ResolveDuplicatedMaterialNames();
	};


	/******************************************************************************************/
	// VMD
	/******************************************************************************************/

	//ソルバー格納用、ボーン名を省略したVMDMotion
	struct VMDMotionLite {
		int iFrame = 0;
		XMFLOAT3 position = {};	//位置
		XMFLOAT4 rotation = { 0,0,0,1 };	//姿勢(クォータニオン)
		union {
			char bezierParams[16];
			struct Bezier {
				char X[4];	//Xax, Xay, Xbx, Xbyの順
				char Y[4];
				char Z[4];
				char R[4];
			} bezier;
		};
		VMDMotionLite() {
			bezier.X[0] = bezier.X[1] = bezier.Y[0] = bezier.Y[1] = bezier.Z[0] = bezier.Z[1] = bezier.R[0] = bezier.R[1] = 20;
			bezier.X[2] = bezier.X[3] = bezier.Y[2] = bezier.Y[3] = bezier.Z[2] = bezier.Z[3] = bezier.R[2] = bezier.R[3] = 107;
		};
		bool physics = true;	//物理ボーンの場合は物理有効/無効フラグ。非物理ボーンの場合は単純に無視される

		bool selected = false;		//編集用
	};

	//読み込み用、ボーン名がANSIのまま格納されている
	struct VMDMotionANSI : VMDMotionLite {
		std::string name;
	};


	//VMDファイルやコピーバッファ格納用。ボーン名入り
	struct VMDMotion : VMDMotionLite {
		std::wstring name;	//ボーン名(VMDは本来SJISなのでUTF-16に変換したもの)
	};

	//ソルバー格納用、モーフ名を省略したVMDMorph
	struct VMDMorphLite {
		int iFrame = 0;
		float value = 0;
		bool selected = false;		//編集用
	};

	struct VMDMorphANSI : VMDMorphLite {
		std::string name;
	};

	//VMDファイルやコピーバッファ格納用。モーフ名入り
	struct VMDMorph : VMDMorphLite {
		std::wstring name;
	};



	//ボーンモーフ(ポーズソルバー用。ファイル格納用ではない)
	struct VMDBoneMorph {
		int iMorph = 0;		//何番のモーフか
		float ratio = 0;	//効果率
		XMFLOAT4 rotation = {};	//回転
		XMFLOAT3 translation = {};	//移動
	};

	//カメラキーフレーム
	struct VMDCamera {
		int iFrame = 0;
		float distance = -45;	//目標点までの距離(目標点がカメラ前面でマイナス)
		XMFLOAT3 target = {0,10,0};	//目標点の位置
		XMFLOAT3 rotation = {};	//各軸周りの回転(おそらくxだけMMDの数値入力のマイナス)
		union {
			struct BezierCam {
				char X[4];	//もともとはtarget.xのベジェ補間パラメータ、ax bx ay byの順だが	ax ay bx by に並び変えてある
				char Y[4];	
				char Z[4];
				char R[4];	//回転の補完パラメータ(各軸共通っぽい)
				char L[4];	//distanceの補間パラメータ
				char V[4];	//視野角の補完パラメータ
			} bezier;
			char bezierParams[24];
		};
		float viewAngle = 30;	//視野角
		bool parth = true;		//パース
		//以下、VMDには入らない情報
		int parentID = -1;	//追従モデルのID(-1で追従なし)
		int parentBone = -1;
		VMDCamera() {
			bezier.X[0] = bezier.X[1] = 20;	bezier.X[2] = bezier.X[3] = 107;
			bezier.Y[0] = bezier.Y[1] = 20;	bezier.Y[2] = bezier.Y[3] = 107;
			bezier.Z[0] = bezier.Z[1] = 20;	bezier.Z[2] = bezier.Z[3] = 107;
			bezier.R[0] = bezier.R[1] = 20;	bezier.R[2] = bezier.R[3] = 107;
			bezier.L[0] = bezier.L[1] = 20;	bezier.L[2] = bezier.L[3] = 107;
			bezier.V[0] = bezier.V[1] = 20;	bezier.V[2] = bezier.V[3] = 107;
		};
		bool selected = false;		//編集用
	};

	//照明キーフレーム
	struct VMDLight {
		int iFrame = 0;
		XMFLOAT3 color = { 154.0f / 255.0f, 154.0f / 255.0f, 154.0f / 255.0f };
		XMFLOAT3 direction = { -0.5f, -1.0f, +0.5f };
		bool selected=false;		//編集用
	};

	//セルフ影キーフレーム
	struct VMDSelfShadow {
		int iFrame = 0;
		int kind = 1;	//セルフシャドウ種類(0:off, 1:mode1, 2:mode2)
		float distance = 0.1125f;	//セルフシャドウ距離(MMDの入力値L→(10000-L)/10000にした値)
		bool selected = false;		//編集用
	};

	//重力設定キーフレーム(独自フォーマット)
	struct VMDGravity {
		int iFrame = 0;
		float g = 9.8f;	//重力の大きさ係数,UIに表示される値(これを10倍した値をBulletに渡すとちょうどよさげ)
		XMFLOAT3 direction = { 0,-1,0 };	//UIに表示される正規化前の値。演算時には正規化して使う
		float noiseAmp = 0;		//ノイズ振幅
		float noiseFreq = 1;	//ノイズ最小周波数[Hz]
		bool selected = false;	//編集用

		//Bullet用の重力ベクトルを計算するframeはフレーム単位の時刻(time=30で1秒)
		btVector3 BtGravity(float frame);
	};

	//IK情報1つ
	struct VMDIK {
		std::wstring name;			//オリジナルのVMDデータはANSIで20バイト
		bool enabled = true;		//IK有効？
	};

	//外部親情報
	struct VMDExParent {
		int parentID = -1;			//外部親モデルID
		std::wstring parentBoneName;//外部親に指定されたボーン名
		std::wstring boneName;		//このモデルのどのボーンが子ボーンになるのか？
		//↑2つをソルバーへのペースト時に展開した物が↓
		int parentBone = -1;	//外部親モデルのどのボーンが親ボーンになるのか？
		int bone = -1;			//このモデルのどのボーンが子ボーンになるのか？
	};

	//表示・IK・外親キー
	struct VMDExtra {
		int iFrame = 0;
		bool show = true;				//表示フラグ
		std::vector<VMDIK>IK;			//IK情報
		bool selected = false;			//編集用
		std::vector<VMDExParent> exp;	//外部親情報一覧。キーフレームには一覧として持つ
	};


	//VMDファイルローダ
	class VMD {
	private:
		UINT m_codepage;	//VMDファイルが作成されたときに使われたコードページ
		VMDMotionANSI FreadMotion(FILE* fp);
		VMDMorphANSI FreadMorph(FILE* fp);
		VMDCamera FreadCamera(FILE* fp);
		VMDLight FreadLight(FILE* fp);
		VMDSelfShadow FreadSelfShadow(FILE* fp);
		VMDExtra FreadExtra(FILE* fp);
	public:
		std::wstring modelName;
		std::vector<VMDMotion>motionKeys;
		std::vector<VMDMorph>morphKeys;
		std::vector<VMDCamera>cameraKeys;
		std::vector<VMDLight>lightKeys;
		std::vector<VMDSelfShadow>selfShadowKeys;
		std::vector<VMDExtra>extraKeys;	//表示・IK・外親キー
		int lastFrame = 0;	//最終フレーム番号
		VMD() {};
		//日本語環境で作成されたVMDファイルを他国語環境で処理したい場合はcodepageに932を指定すべし
		//target : 読み込み対象モデル
		// codepageにボーン名・モーフ名それぞれが変換した時に先頭15バイトがマッチしていれば「同名のボーン/モーフ」として変換する
		// 読み込み対象モデルを指定しない(nullptr)場合は15バイト以上のボーン・モーフ名を含む場合にマッチしないデータとして処理を続行する
		VMD(const wchar_t* fname, UINT codepage = CP_ACP, const PMX::PMXModel* target = nullptr);
		~VMD();
		//モーション・モーフを含まず、カメラのキーを含むvmdファイルならtrueを返す
		static bool IsCameraVMDFile(const wchar_t* filename);
		//VMDファイル書き出し、camera:カメラ用データならtrue
		void Save(bool camera, const std::wstring& filename, UINT codepage = CP_ACP);
	};
	

	/******************************************************************************************/
	// 物理 : benikabocha様作 sabaを 大いに参考にさせていただきました
	/******************************************************************************************/
	class PoseSolver;
	class PoseBone;
	class Physics;

	class MMDMotionState : public btMotionState {
	public:
		virtual void Reset() = 0;
		virtual void ReflectGlobalTransform() = 0;
	};


	class RigidBody {
	private:
		XMMATRIX m_offsetMat;
		Physics* m_physics;
		std::unique_ptr<btCollisionShape>	m_shape;
		std::unique_ptr<MMDMotionState>		m_activeMotionState;
		std::unique_ptr<MMDMotionState>		m_kinematicMotionState;
	public:
		PoseBone* bone;
		int mode;	//0:ボーン追従(static) 1:物理演算(dynamic) 2:物理演算 + Bone位置合わせ
		int group;
		int groupMask;
		bool physKey = true;	//物理有効キー(Solverから変更される)
		std::unique_ptr<btRigidBody> rigidBody;
		RigidBody(const PMXModel* model, int iRB, const PoseSolver* solver);	//モデルデータと剛体番号とポーズソルバの参照を渡して物理剛体の作成
		void SetActivation(bool active);
		void ResetTransform();
		void Reset();
		void ReflectGlobalTransform();
		void CalcLocalTransform();	//親への変換を計算してm_boneにセットする
		XMMATRIX GetTransform();
	};

	class Joint {
	public:
		int src;	//PMX内での番号
		std::unique_ptr<btTypedConstraint> constraint;
		Joint(const PMXModel* model, int index, RigidBody* rigidBodyA, RigidBody* rigidBodyB);
	};

	//Bulletを用いた物理ソルバー
	class Physics {
	private:
		std::unique_ptr<btBroadphaseInterface>				m_broadphase;
		std::unique_ptr<btDefaultCollisionConfiguration>	m_collisionConfig;
		std::unique_ptr<btCollisionDispatcher>				m_dispatcher;
		std::unique_ptr<btSequentialImpulseConstraintSolver>m_solver;
		std::unique_ptr<btDiscreteDynamicsWorld>			m_world;
		std::unique_ptr<btCollisionShape>					m_groundShape;
		std::unique_ptr<btMotionState>						m_groundMS;
		std::unique_ptr<btRigidBody>						m_groundRB;
		std::unique_ptr<btOverlapFilterCallback>			m_filterCB;
		std::vector<PoseSolver*> m_notifees;	//なんか動きました、という事を知らされる人たち
		//剛体・ジョイントの追加・削除…PoseSolverのAttach/DetachPhysicsから呼ばれる
		void AddRigidBody(RigidBody* rb);
		void RemoveRigidBody(RigidBody* rb);
		void AddJoint(Joint* joint);
		void RemoveJoint(Joint* joint);
		//Update,Resetした時に通知を受けるソルバーの追加・削除…PoseSolverのAttach/DetachPhysicsから呼ばれる
		void AddNotifee(PoseSolver* solver);
		void DeleteNotifee(PoseSolver* solver);
		double m_dt = 0;	//最後にUpdateを実行した時にセットされたdt
	public:
		Physics();
		~Physics();
		double fps = 120;
		double maxSubsteps = 10;
		void Reset();
		void Update(double dt, bool active = true);	//active=falseの時、キーフレームなどの状態にかかわらず全物理ボーンをkinematicな物として更新する
		void Prewarm(int frames, float timePerFrame = 1 / 30.0f);
		btDiscreteDynamicsWorld* World() { return m_world.get(); }
		//地面の当たり判定on/off
		void GroundEnable(bool enable);
		inline bool GroundEnable() const { return !(m_groundRB->getCollisionFlags() & btCollisionObject::CF_NO_CONTACT_RESPONSE); }
		inline double dtime() const { return m_dt; }
		const std::vector<PoseSolver*>& Notifiees() const { return m_notifees; }
		btContactSolverInfo& GetSolverInfo() { return m_world->getSolverInfo(); }
		friend RigidBody;
		friend Joint;
		friend PoseSolver;
	};


	/******************************************************************************************/
	// PoseSolver
	/******************************************************************************************/
	class PoseSolver;

	//各ボーンの変換を計算するためのクラス。PoseSolverの中で使われる
	class PoseBone {
	public:
		PoseSolver* owner;
		//コンストラクタでセットされたら変わらない情報群
		int iBone = -1;		//このボーンのPMX内での番号
		XMFLOAT3 origin;	//ボーンの回転中心(ワールド座標系)
		PoseBone* parent = nullptr;			//親ボーン
		std::vector<PoseBone*> children;	//子ボーン群
		
		//キーフレーム情報:モーション読み込み時に決定。キーフレームの値とボーンモーフの値を合算した物
		XMFLOAT3 keyTranslation;		//キーフレームに書いてある移動分
		XMVECTOR keyRotation;			//キーフレームに書いてある回転分
		
		//IKと付与計算用
		bool IKLinked = false;			//IKリンクとして動かされたならTrue
		XMVECTOR IKRotation;			//IKリンクとして動かされた回転量
		XMFLOAT3 appendedTranslation;	//付与された移動分
		XMVECTOR appendedRotation;		//付与された回転分

		//テスト中、付与+IKボーン用
		int IKMaster = -1;				//自分がIKリンクボーンだった場合、評価すべきIKボーン。リンクボーン以外では-1
		
		//以上から計算されるローカル変換とワールド変換
		XMFLOAT3 localTranslation;	//key,appendから計算されるローカル移動分
		XMVECTOR localRotation;		//key,IK,appendから計算されるローカル回転分

		void UpdateLocalTransform();	//localTranslation/RotationとoriginからtoParentを計算し、「ワールド変換はまだ更新されていない」フラグを立てる
		XMMATRIX toParent;				//↑で計算される、親ボーン座標系への変換行列(ローカル変形行列)

		bool transformResolved = false;	//ワールド変換が有効である、フラグ
		XMMATRIX transform;			//ワールド座標系までの変換行列

		bool ExternalParent(XMMATRIX& transform);
		void Propagate(PoseBone* bone);
		void PropagateUnresolved(PoseBone* bone);	//変換が更新された旨だけ子ボーンに伝える
		XMMATRIX ResolveTransform();	//ワールド変換を得る。transformが未確定の場合、解決する

		PoseBone();
		~PoseBone();
	};


	//材質モーフによる変化分(Solveごとに変更される)
	struct MaterialMorph {
		XMFLOAT4 diffuse = {};
		XMFLOAT3 specular = {};
		float shininess = 0;
		XMFLOAT3 ambient = {};
		XMFLOAT4 edgeColor = {};
		float edgeSize = 0;
		//テクスチャ材質モーフ
		XMFLOAT4 textureValue = {};
		XMFLOAT4 sphereValue = {};
		XMFLOAT4 toonTextureValue = {};
	};

	//材質モーフ適用後のマテリアル(Solveごとに変更される)
	struct Material {
		XMFLOAT4 diffuse = {};
		XMFLOAT3 specular = {};
		float shininess = 0;
		XMFLOAT3 ambient = {};
		XMFLOAT4 edgeColor = {};
		float edgeSize = 0;
		//テクスチャ材質モーフ
		XMFLOAT4 textureAddValue = {};
		XMFLOAT4 sphereAddValue = {};
		XMFLOAT4 toonTextureAddValue = {};
		XMFLOAT4 textureMulValue = {1,1,1,1};
		XMFLOAT4 sphereMulValue = {1,1,1,1};
		XMFLOAT4 toonTextureMulValue = {1,1,1,1};
		//以下はモデル読み込み後変わらない項目
		int drawFlag = 0;	//描画ﾌﾗｸﾞ 0x01:両面, 0x02:地面影, 0x04:セルフシャドウキャスターになる, 0x08:セルフシャドウ描画, 0x10:エッジ描画
		int tex = 0;	//テクスチャ番号
		int spTex = 0;
		int spmode = 0;	//スフィアモード 0:無効 1:乗算, 2:加算 3:サブテクスチャ
		int toonFlag = 0;	//共有Toonフラグ 0:継続値は個別Toon 1:継続値は共有Toon
		int toonTex = 0;	//テクスチャインデックス、または共有toonテクスチャ番号(0～9がtoon01.bmp～toon10.bmpに対応)
		int vertexCount = 0;	//頂点数(必ず3の倍数)
		Material() {}
		Material (const PMXMaterial& p) {
			diffuse = p.diffuse;
			specular = p.specular;
			shininess = p.shininess;
			ambient = p.ambient;
			edgeColor = p.edgeColor;
			edgeSize = p.edgeSize;
			drawFlag = p.drawFlag;
			tex = p.tex;
			spTex = p.spTex;
			spmode = p.spmode;
			toonFlag = p.toonFlag;
			toonTex = p.toonTex;
			vertexCount = p.vertexCount;
		}
	};

	//モーションデータの部分集合を入れるためのコンテナ
	struct KeySubset {
		std::vector<VMDMotion>motions;
		std::vector<VMDMorph>morphs;
		std::vector<VMDExtra>extras;
		std::vector<VMDCamera>cameras;
		std::vector<VMDLight>lights;
		std::vector<VMDSelfShadow>selfShadows;
		std::vector<VMDGravity>gravities;
		KeySubset() {};
		KeySubset(const VMD* vmd);	//VMDオブジェクトから作成
		void SelectKeys(bool select = true);	//格納されている全キーの選択状態をselectに変更
		int FirstFrame() const;	//サブセットに納められているキーのうち最初のフレーム番号
		int LastFrame() const;	//サブセットに納められているキーのうち最後のフレーム番号
		bool Empty() const;		//空のサブセットならtrueを返す
		void LoadVPD(const std::wstring& filename, UINT codepage);
		VMD MakeVMD(const std::wstring& modelname) const;
		//外部親情報のリターゲット
		//selectedがtrueになっているキーの外部親情報について、以下を行う
		//①after.parentID/parentBone/boneのいずれかが0未満である場合、before.parentIDが一致しているキーを削除する
		//②そうでない場合、beforeのparentID, parentBoneName, boneNameが全部一致している場合、afterのキーに置換する
		//IK,表示には影響しない
		void RetargetExternalKeys(const VMDExParent& before, const VMDExParent& after);
		bool HasExternalKeys() const;	//外部親キーを含むか？
	};

	//Undo,Redoバッファ ... PoseSolver::Revset, Revdelsetで作られる
	//キーフレームに対する操作は①サブセットからのキーのペースト(paste)、②キーの削除(delete) この二種類しかない
	//**Solver::Undo()に指定されるとキー群の削除→キー群の書き込みの順で実行する
	//以下の説明は、except0fフラグについて考慮してないように見えるが、except0fフラグはdeleteとpasteの混合として実装される
	struct KeyUndo {
		//まずこのキーを削除せよ、という情報
		//pasteの場合  | redo:空 , undo:pasteによって新たに作られたキー
		//deleteの場合 | redo:delete対象になるキー undo:空
		std::vector<std::unordered_map<int, bool>> motionSel, morphSel;
		std::unordered_map<int, bool> extraSel, cameraSel, lightSel, selfShadowSel, gravitySel;
		//次にこのキーを書き込め、という情報
		//pasteの場合  | redo:サブセットの内容, undo:上書されたキーの上書前の状態
		//deleteの場合 | redo:空, undo:削除前の削除対象キー
		KeySubset rev;
	};


	//PoseSolver用補間したキーフレーム
	//
	struct Pose {
		std::vector<VMDMotionLite> boneKeys;
		std::vector<VMDMorphLite> morphKeys;
		VMDExtra extraKey;
	};


	enum class SolverType { none, pose, camera };

	//カメラ・ポーズソルバ共通インターフェイス(キーフレーム編集用のメソッドの集合)
	class ISolver {
	public:
		SolverType type = SolverType::none;
		
		//selectedがtrueになっているキーをsubsetに格納する
		//trimがtrueの場合、モーションキー・モーフキーのうち先頭に書いてあるキーフレームを0フレームとした相対フレーム番号で記述する
		virtual void Subset(KeySubset& subset, bool trim) = 0;
		
		//Subsetで作成したサブセットを結合する。他のsolverのsubsetでもよい(ボーン・表情名でマッチングする)
		//frameは結合先の先頭フレーム
		//例 : フレーム#100のデータが入っていた場合、frame + 100番のフレームに書き込まれる
		virtual void Join(const KeySubset& subset, int frame) = 0;

		//subsetをframeにペーストした際に巻き戻すための情報をundo, やり直すための情報をredoに作成する
		virtual void Revset(const KeySubset& subset, int frame, KeyUndo& undo, KeyUndo& redo) = 0;
		
		//selectedがtrueになっているキーを削除する時のアンドゥ・リドゥ情報を作る
		//except0fがtrueの時は0フレームのキーが選択状態になっている場合、キーの初期値を代入する
		virtual void Revdelset(bool except0f, KeyUndo& undo, KeyUndo& redo) = 0;

		//undoを使って巻き戻す(redoを指定するとやり直しが出来る)
		virtual void Undo(const KeyUndo& undo) = 0;
		
		//1つでもキーがselectedならtrue
		virtual bool AnySelected() = 0;

		//最後にキーの打ってあるフレーム番号を得る(0フレームには必ずキーが打ってあるので最初のフレームは0)
		virtual int LastFrame() = 0;

		//selectedがtrueになっているキーを削除する
		virtual void DeleteKeys(bool except0f = true) {
			KeyUndo undo, redo;
			Revdelset(except0f, undo, redo);
			Undo(redo);
		}

		//全キーの選択状態をselectにする
		virtual void SelectAllKeys(bool select) = 0;

		//外部親・追従親に関する情報を削除
		virtual void PurgeExternalKeys(int purgedID) = 0;
	};

	//リンクボーンがどんな関節を作っているのか?
	struct IKLinkJoint {
		PoseBone* bone;		//リンクボーンを作るボーンへの参照
		XMVECTOR originalRotation;	//IK計算前の回転分
		bool limit = false;	//無制限に動ける
		bool hinge = false;	//1軸可動
		bool fixed = false;	//可動範囲が無い
		XMFLOAT3 hingeAxis = { 1,0,0 };
		XMFLOAT3 hi = { 3.1415927f, 3.1415927f, 3.1415927f };
		XMFLOAT3 lo = { -3.1415927f, -3.1415927f, -3.1415927f };
		float hingeLo = -3.1415927f; //ヒンジの場合の可動範囲
		float hingeHi = 3.1415927f;
		IKLinkJoint() {};
		IKLinkJoint(const PMXIKLink& l);
		//ローカル座標系の姿勢を制限する
		SimpleMath::Quaternion Limit(const SimpleMath::Quaternion& q) const;
	};

	//pmxにvmdを割り当てて、あるフレームにおけるポーズ(=各ボーンの変換行列)を計算・格納するクラス
	//表示したいpmxモデル1体につき1つ生成する必要があります
	class PoseSolver : public ISolver {

		void IK(int iBone);		//Solveから呼ばれる。1つのIKボーンを解決する
		void FinishMatrices();	//bonesをboneMatricesに反映する

		//物理対応
		void AttachPhysics();			//コンストラクタで呼ばれる
		void DetachPhysics();			//デストラクタで呼ばれる

		//↓コンストラクタでセットされたら変わらないメンバ
		std::vector<int> TFOrder;	//各ボーンの変形順序テーブル。transformOrder[0]に最初に変形されるボーン番号を格納
		int bonesBeforePhys;		//物理前変形ボーンの本数
		std::vector<std::vector<IKLinkJoint>> m_IKJoints;	//IKリンクのための関節情報

		//物理プリウォーム用
		std::vector<XMFLOAT3>m_goalT;
		std::vector<XMVECTOR>m_goalQ;

		//物理対応用
		//physicsから参照されるモノ
		//std::vector<PoseBone*>bones;	//PoseBoneはそのままPMXのボーン番号と対応している
		PoseBone* root = nullptr;		//-1番のボーン(全ての親の親)
		Physics* physics = nullptr;
		void OnUpdatePhysicsBegin(bool active);	//active = falseの時、キーフレームなどの状態によらず全部の物理ボーンをkinematicとして扱う
		void OnUpdatePhysicsEnd();
		void OnResetPhysicsBegin();
		void OnResetPhysicsEnd();
		void OnPrewarmPhysics(int frames, int totalFrames);	//framesフレーム掛けて標準状態から最後にSolveされた状態までもっていく
		
		//材質モーフ計算用
		std::vector<MaterialMorph> materialMorphAdd;
		std::vector<MaterialMorph> materialMorphMul;

		//インパルスモーフ計算用(剛体の数だけある)
		std::vector<XMFLOAT3> impulseLinear;
		std::vector<XMFLOAT3> impulseAngular;
		std::vector<XMFLOAT3> impulseLinearLocal;
		std::vector<XMFLOAT3> impulseAngularLocal;
		std::vector<bool> impulseReset;

		//デフォルトモーフ値
		std::vector<float> m_morphDefaults;
	public:
		const PMXModel* pmx;

		//Solveから呼ばれる関数
		//iFrameにおけるボーン番号iBoneについての位置・向き・物理on/offをモーションデータから得る。contPhysについてはSolveの説明を参照
		void GetKeyFrame(float time, int iBone, XMFLOAT3& translation, XMVECTOR& rotation, bool& phys, bool contPhys);
		float GetKeyFrameMorph(float time, int iMorph); //↑のモーフ版
		VMDExtra GetKeyFrameExtra(float time);			//↑の表示・IK・外親版
		VMDExtra defaultExtraKey;						//全IK有効、表示状態onになっているデフォルトのVMDExtraキー

		std::vector<PoseBone*>bones;	//PoseBoneはそのままPMXのボーン番号と対応している

		//コントスラクタ
		// _physicsがnullptrの場合、物理演算しない
		// _pmxがnullptrではダメ
		// _vmdがnullptrの場合、0フレームに初期状態のみが書かれたキーフレームで初期化される
		// PoseSolverのライフサイクルの間、pmx、physicsは解放されると困る
		PoseSolver(Physics* _physics, const PMXModel* _pmx, const VMD* vmd);
		~PoseSolver();

		//コピー禁止(PoseBoneのコピーが面倒なので)
		PoseSolver(const PoseSolver&) = delete;
		PoseSolver& operator = (const PoseSolver&) = delete;

		Pose solvedPose;	//最後にSolveされた時のポーズ(物理適用前)
		Pose physPose;		//最後にUpdatePhysicsされた時のポーズ(物理適用後)
		VMDExtra miningExtra;	//マイニング中のポーズにおける外部親状態取得用

		//ハイ、ポーズ！
		//キーフレーム群を適宜補間して時刻timeにおけるポーズをposeに格納する。開始フレームはtime=0
		//timeの単位はMMDでの1フレームにあたる1/30秒。time = 30であればモーションの開始から1秒後の状態を示す
		//integerTimeがtrueの時はtimeの小数を切り捨て、当該フレームにキーが打ってある場合は補間無しで格納する(ほぼ編集時用)
		//poseに格納される補間済みのキーフレームからは補間曲線のデータは得られない
		//integerTimeがtrueの時の補間無しのポーズに対しては、あれば元データからコピーされる
		//contPhys : Solveを参照
		void HiPose(Pose& pose, float time, bool integerTime, bool contPhys);

		//ポーズデータを元に各ボーンのIKやグループモーフの展開などを行ってbones,morphValuesなどに反映する
		//※PoseMiningを行うと物理反映前の状態になる。physics.Update()で物理反映後の状態になる
		//※TODO:物理後変形対応
		void PoseMining(const Pose& pose);

		//時刻timeにおけるポーズをbonesに、モーフをmorphValuesに反映する。開始フレームはtime=0
		//timeの単位はMMDでの1フレームにあたる1/30秒。time = 30であればモーションの開始から1秒後の状態を示す
		//integerTime : trueの時はtimeの少数を切り捨て、当該フレームにキーが打ってある場合は補間無しの状態を得る(30fpsで再生する時用)
		//contPhys : 物理演算の取り扱い用フラグ
		// trueの時はフレームの再生は連続している事を前提にする
		// 　物理キーon→offのフレームでは最後にonになった時の物理演算結果から逆算されたキー(physPose)と、次フレームの補間にする(kinematicな状態になる)
		// falseの時は編集時などフレームの再生が飛び飛びである事を前提にする。連続再生時も最初のフレームはfalseを指定すべし
		//   物理on/offの状態は純粋に直前のフレームを参照する
		// 連続再生時に最初のフレームにおいてはfalseを指定しなければならない理由は、再生直後においてはphysPoseが未定義になっているため
		void Solve(float time, bool integerTime = false, bool contPhys = false);

		//↓Solve(PoseMining)の結果を反映するメンバ。再生専用のアプリケーションからは以下の4つを参照すれば大体OK
		std::vector<float>morphValues;		//モーフ値。pmxのモーフ番号がそのまま対応しているが、グループモーフは展開されている
		std::vector<XMMATRIX>boneMatrices;	//各ボーンのグローバル変形を格納した配列(シェーダに渡せるよう転置済み)
		std::vector<Material>materials;		//材質モーフ適用後の材質
		bool visible = true;				//表示状態になってる？

		
		//物理演算の状態を見るため用
		std::vector<RigidBody*> body;	//読み込んだpmxモデルに応じた物理演算用オブジェクト
		std::vector<Joint*> joint;

		//keywordsにマッチ(半角の場合は大文字・小文字は区別しない)する部分文字列を一つ以上持っているジョイントの可動域が0だった場合
		//移動分については±linear、回転分については±angularまで緩和する
		//※MMDで採用しているBullet2.75では可動範囲の下限と上限が一致しているconstraintも多少遊びがあったが
		//　Bullet3ではそのようなconstraintは他の剛体が当たらない限りはほぼ動かないので動きが硬くなってしまうのでそれを補償する
		void RelaxJoints(const std::vector<std::wstring>& keywords, const XMFLOAT3& linear, const XMFLOAT3& angular);
		//指定したインデックスのジョイントを緩和する
		void RelaxJoint(int iJoint, const XMFLOAT3& linear, const XMFLOAT3& angular);

		/*** 以下、編集用のデータやメソッド ***/
		std::unordered_map<std::wstring, int> boneDict, morphDict;	//名前→インデックス辞書
		std::vector<std::map<int,VMDMotionLite>> motionKeys;//ボーンごとに分類したフレームキー motions[ボーン番号][フレーム]
		std::vector<std::map<int,VMDMorphLite>> morphKeys;	//モーフごとに分類したフレームキー morphs[モーフ番号][フレーム]
		std::map<int, VMDExtra>extraKeys;				//表示・IK・外親キー　IKキーはモデルのIKボーンの数だけ必ずある

		//IKEnabled[]の添え字を得るためのテーブル。コンストラクタでセットされたら変わらない
		std::vector<int>IK2Bone;					//IKボーン番号→全体ボーン番号テーブル
		std::vector<int>Bone2IK;					//全体ボーン番号→IKボーン番号テーブル
		
		std::vector<bool>IKEnabled;					//Solveごとに変わる。各IKボーンは有効か？IKEnabled[Bone2IK[bone]]でbone番のボーンのIK有効・無効を得られる
		bool IKBoneEnabled(int iBone);				//↑をラップしたユーティリティ。IKボーンの有効/無効(iBone番のボーンがそもそもIKボーンではない場合はfalseが返る)

		std::unordered_map<int, PoseSolver*> externalSolver;	//外部親を参照するためのテーブル、モデルの追加・削除が行われた場合、全部のsolverについてこれを更新せねばならない

		void PurgeExternalKeys(int purgedID);	//外部親になっているモデルが破棄された可能性があるのでextraKeysをスキャンして該当するキーから外部親情報を削除する

		//以下、編集用インターフェイスからの継承メソッド
		bool AnySelected() override;
		void Subset(KeySubset& subset, bool trim) override;
		void Revset(const KeySubset& subset, int frame, KeyUndo& undo, KeyUndo& redo) override;
		void Undo(const KeyUndo& undo) override;
		void Revdelset(bool except0f, KeyUndo& undo, KeyUndo& redo) override;
		void Join(const KeySubset& subset, int frame) override;
		void SelectAllKeys(bool select);
		int LastFrame() override;

		//反転P用サブセットの作成
		void LRset(KeySubset& subset);

		//デフォルトモーフ値のセット, 該当モーフが無い場合はfalseを返す, set0fがtrueの時、0Fのキーにデフォルト値をセットする
		bool SetDefaultMorphValue(const std::wstring& morphName, float defaultValue, bool set0f);

		friend Physics;
		friend RigidBody;
		friend Joint;
	};


	//カメラ・光源・セルフ影についてのソルバ
	class CameraSolver : public ISolver{
	public:
		std::map<int, VMDCamera>cameraKeys;
		std::map<int, VMDLight>lightKeys;
		std::map<int, VMDSelfShadow>selfShadowKeys;
		std::map<int, VMDGravity>gravityKeys;

		CameraSolver() {};
		CameraSolver(VMD* vmd);
		//時刻timeにおけるポーズをbonesに、モーフをmorphValuesに反映する。開始フレームは0
		//timeの単位はMMDでの1フレームにあたる1/30秒。time = 30であればモーションの開始から1秒後の状態を示す
		void Solve(float time);

		//projectionMatrixを作る時に必要な情報
		float aspect = 16.0f / 9.0f;	//画面の横/縦比…レンダーターゲットのサイズが変更されたらここも変えないといかんよ
		float nearZ = 1.0f;				//近方クリッピング面、projectionMatrixを計算するために必要
		float farZ = 100000.0f;			//遠方クリッピング面、projectionMatrixを計算するために必要

		//↓Solveの結果を反映するメンバ
		VMDCamera camera;
		VMDSelfShadow selfshadow;
		VMDLight light;					//注意：directionは正規化されていない。キーフレームの値そのまま
		VMDGravity gravity;				//注意：directionは正規化されていない。キーフレームの値そのまま
		//シェーダに転送する用。転置済みのビュー・プロジェクション行列
		XMMATRIX viewMatrix, projectionMatrix;
		//あれば使えるけどcameraには直接入ってない物
		XMFLOAT3 cameraPosition;
		//Bulletに指定すべき重力ベクトル
		btVector3 btGravity;
		//補間が効いてればtrue(カメラのキーフレームが時間的に隣接して打たれている場合は補間しない)
		bool interpolated = false;

		//ユーティリティ
		//カメラパラメータから行列とカメラ位置を計算する。行列は転置済み(シェーダに渡せる)
		static void Matrices(const VMDCamera& cam, XMMATRIX& view, XMMATRIX& proj, XMFLOAT3& pos, float aspect, float nearZ = 1, float farZ = 1e+5);	
		//カメラ追従対応版、boneにカメラの追従対象ボーンのワールド行列、boneOrigineにカメラの追従対象ボーンの回転中心を入れる
		static void TrackingMatrices(const XMMATRIX& bone, const XMFLOAT3& boneOrigin, const VMDCamera& cam, XMMATRIX& view, XMMATRIX& proj, XMFLOAT3& pos, float aspect, float nearZ = 1, float farZ = 1e+5);
		

		//編集用
		bool AnySelected() override;
		void Subset(KeySubset& subset, bool trim) override;
		void Revset(const KeySubset& subset, int frame, KeyUndo& undo, KeyUndo& redo) override;
		void Undo(const KeyUndo& undo) override;
		void Join(const KeySubset& subset, int frame) override;
		void Revdelset(bool except0f, KeyUndo& undo, KeyUndo& redo) override;
		void SelectAllKeys(bool select);
		int LastFrame() override;
		void PurgeExternalKeys(int purgedID);	//カメラの追従対象になっているモデルが破棄された可能性があるのでcameraKeysをスキャンして該当するキーから外部親情報を削除する
	};



	namespace Math {
		using namespace DirectX;
		using namespace DirectX::SimpleMath;
		/***********************************************************************************/
		//数学系ユーティリティ SimpleMathにさらにちょい足し
		/***********************************************************************************/
		constexpr float PI = 3.14159265359f;
		typedef Vector2 vec2;
		typedef Vector3 vec3;
		typedef Vector4 vec4;
		typedef Quaternion vecQ;

		inline Matrix operator*(const Matrix& M, const vecQ& q) { return M * Matrix::CreateFromQuaternion(q); }
		inline Matrix operator*(const vecQ& q, const Matrix& M) { return Matrix::CreateFromQuaternion(q) * M; }

		inline float Lerp(float a, float b, float t) { return a * (1 - t) + b * t; }
		inline vec2 Lerp(vec2 a, vec2 b, vec2 t) { return vec2(Lerp(a.x, b.x, t.x), Lerp(a.y, b.y, t.y)); };
		inline vec3 Lerp(vec3 a, vec3 b, vec3 t) { return vec3(Lerp(a.x, b.x, t.x), Lerp(a.y, b.y, t.y), Lerp(a.z, b.z, t.z)); };
		inline vec4 Lerp(vec4 a, vec4 b, vec4 t) { return vec4(Lerp(a.x, b.x, t.x), Lerp(a.y, b.y, t.y), Lerp(a.z, b.z, t.z), Lerp(a.w, b.w, t.w)); };
		inline vecQ Lerp(vecQ a, vecQ b, vecQ t) { return vecQ(Lerp(a.x, b.x, t.x), Lerp(a.y, b.y, t.y), Lerp(a.z, b.z, t.z), Lerp(a.w, b.w, t.w)); };
		inline float Clamp(float a, float mi, float ma) { return min(max(a, mi), ma); }
		inline vec2 Clamp(const vec2& a, const vec2& mi, const vec2& ma) { return vec2(Clamp(a.x, mi.x, ma.x), Clamp(a.y, mi.y, ma.y)); }
		inline vec3 Clamp(const vec3& a, const vec3& mi, const vec3& ma) { return vec3(Clamp(a.x, mi.x, ma.x), Clamp(a.y, mi.y, ma.y), Clamp(a.z, mi.z, ma.z)); }
		inline vec4 Clamp(const vec4& a, const vec4& mi, const vec4& ma) { return vec4(Clamp(a.x, mi.x, ma.x), Clamp(a.y, mi.y, ma.y), Clamp(a.z, mi.z, ma.z), Clamp(a.w, mi.w, ma.w)); }
		inline float Step(float y, float x) { return x>=y; }
		inline vec2 Step(const vec2& y, const vec2& x) { return vec2(Step(y.x, x.x), Step(y.y, x.y)); }
		inline vec3 Step(const vec3& y, const vec3& x) { return vec3(Step(y.x, x.x), Step(y.y, x.y), Step(y.z, x.z)); }
		inline vec4 Step(const vec4& y, const vec4& x) { return vec4(Step(y.x, x.x), Step(y.y, x.y), Step(y.z, x.z), Step(y.w, x.w)); }

		inline float SmoothStep(float e0, float e1, float x) {
			if (e0 == e1) return Step(e0,x);
			float a = Clamp((x - e0) / (e1 - e0), 0,1);
			return a * a * (3 - 2 * a);
		}
		inline vec2 SmoothStep(const vec2& e0, const vec2& e1, const vec2& x) { return vec2(SmoothStep(e0.x, e1.x, x.x), SmoothStep(e0.y, e1.y, x.y)); }
		inline vec3 SmoothStep(const vec3& e0, const vec3& e1, const vec3& x) { return vec3(SmoothStep(e0.x, e1.x, x.x), SmoothStep(e0.y, e1.y, x.y), SmoothStep(e0.z, e1.z, x.z)); }
		inline vec4 SmoothStep(const vec4& e0, const vec4& e1, const vec4& x) { return vec4(SmoothStep(e0.x, e1.x, x.x), SmoothStep(e0.y, e1.y, x.y), SmoothStep(e0.z, e1.z, x.z), SmoothStep(e0.w, e1.w, x.w)); }
		auto SmoothClamp = [&](const auto& a, const auto& mi, const auto& ma) { return Lerp(mi, ma, SmoothStep(mi, ma, a)); };

		inline float Sign(float x) {
			if (x > 0) return 1;
			else if (x < 0) return -1;
			else return 0;
		}

		//行ベクトル
		inline vec3 _11_12_13(const Matrix& M) { return vec3(M._11, M._12, M._13); }
		inline vec3 _11_12_13(Matrix& M, const vec3& v) { M._11 = v.x; M._12 = v.y; M._13 = v.z; return v; }
		inline vec3 _21_22_23(const Matrix& M) { return vec3(M._21, M._22, M._23); }
		inline vec3 _21_22_23(Matrix& M, const vec3& v) { M._21 = v.x; M._22 = v.y; M._23 = v.z; return v; }
		inline vec3 _31_32_33(const Matrix& M) { return vec3(M._31, M._32, M._33); }
		inline vec3 _31_32_33(Matrix& M, const vec3& v) { M._31 = v.x; M._32 = v.y; M._33 = v.z;  return v;}
		inline vec3 _41_42_43(const Matrix& M) { return vec3(M._41, M._42, M._43); }
		inline vec3 _41_42_43(Matrix& M, const vec3& v) { M._41 = v.x; M._42 = v.y; M._43 = v.z;  return v;}
		//列ベクトル
		inline vec3 _11_21_31(const Matrix& M) { return vec3(M._11, M._21, M._31); }
		inline vec3 _11_21_31(Matrix& M, const vec3& v) { M._11 = v.x; M._21 = v.y; M._31 = v.z; return v; }
		inline vec3 _12_22_32(const Matrix& M) { return vec3(M._12, M._22, M._32); }
		inline vec3 _12_22_32(Matrix& M, const vec3& v) { M._12 = v.x; M._22 = v.y; M._32 = v.z; return v; }
		inline vec3 _13_23_33(const Matrix& M) { return vec3(M._13, M._23, M._33); }
		inline vec3 _13_23_33(Matrix& M, const vec3& v) { M._13 = v.x; M._23 = v.y; M._33 = v.z; return v; }
		//対角
		inline vec3 _11_22_33(const Matrix& M) { return vec3(M._11, M._22, M._33); }
		inline vec3 _11_22_33(Matrix& M, const vec3& v) { M._11 = v.x; M._22 = v.y; M._33 = v.z; return v; }

		//クォータニオンとV4からxyz成分抜き出し
		inline vec3 xyz(const vecQ& q) { return vec3(q.x, q.y, q.z); }
		inline vec3 xyz(const vec4& v) { return vec3(v.x, v.y, v.z); }
		//vec3とスカラーでvec4を作る
		inline vec4 xyzw(const vec3& i, float w) { return vec4(i.x, i.y, i.z, w); }

		//なんか微妙に使いづらい関数を使いやすくする関数
		inline vec2 Normalize(const vec2& v) { vec2 r; v.Normalize(r); return r; }
		inline vec3 Normalize(const vec3& v) { vec3 r; v.Normalize(r); return r; }
		inline vecQ Normalize(const vecQ& q) { vecQ r; q.Normalize(r); return r; }
		inline vecQ Conjugate(vecQ q) { vecQ r; q.Conjugate(r); return r; }
		inline float Length(const vec2& v) { float f; f = v.Length();  return f; }
		inline float Length(const vec3& v) { float f; f = v.Length();  return f; }
		inline float Length(const vecQ& v) { float f; f = v.Length();  return f; }
		inline float Dot(const vec2& a, const vec2& b) { return a.Dot(b); }
		inline float Dot(const vec3& a, const vec3& b) { return a.Dot(b); }
		inline float Dot(const vec4& a, const vec4& b) { return a.Dot(b); }
		inline float Dot(const vecQ& a, const vecQ& b) { return a.Dot(b); }
		inline Vector3 Cross(const vec3& a, const vec3& b) { return a.Cross(b); }
		inline vec3 NormalizeSafe(const vec3& v) { float s = Dot(v, v); if (s < 1e-12) return vec3(0, 0, 1); else return Normalize(v); }
		inline vecQ NormalizeSafe(const vecQ& q) { float s = Dot(q, q); if (s < 1e-12) return vecQ::Identity; else return Normalize(q); }

		inline vec3 Rotate(const vec3& v, const vecQ& q) { return vec3::TransformNormal(v, Matrix::CreateFromQuaternion(q)); }
		inline vec3 Rotate(const vec3& v, const Matrix& m) { return vec3::TransformNormal(v, m); }
		inline vec4 Rotate(const vec4& v, const vecQ& q) { return vec4::Transform(v, Matrix::CreateFromQuaternion(q)); }
		inline vec3 Transform(const vec3& v, const Matrix& m) { return vec3::Transform(v, m); }
		inline vec4 Transform(const vec4& v, const Matrix& m) { return vec4::Transform(v, m); }


		//Aの回転要素を抜き出した行列を作る
		inline Matrix ExtractRot(const Matrix& A)
		{
			Matrix m;
			m._11 = A._11; m._12 = A._12; m._13 = A._13; m._14 = 0;
			m._21 = A._21; m._22 = A._22; m._23 = A._23; m._24 = 0;
			m._31 = A._31; m._32 = A._32; m._33 = A._33; m._34 = 0;
			m._41 = 0;	m._42 = 0; m._43 = 0; m._44 = 1;
			return m;
		}

		inline vec4 Rotate(const vec4& v, const Matrix& m) { return vec4::Transform(v, ExtractRot(m)); }

		//Aの回転成分をBの回転成分で置き換える
		inline void ReplaceRot(Matrix& A, const Matrix& B)
		{
			A._11 = B._11; A._12 = B._12; A._13 = B._13;
			A._21 = B._21; A._22 = B._22; A._23 = B._23;
			A._31 = B._31; A._32 = B._32; A._33 = B._33;
		}

		//o周りに回る、回転要素がqで平行移動要素がtの行列を作る
		inline Matrix oRtMatrix(const vec3& o, const vecQ& q, const vec3& t)
		{
			//Matrix m = Matrix::CreateTranslation(-o) * q * Matrix::CreateTranslation(o + t); と同値だが早い

			XMMATRIX m = Matrix::CreateFromQuaternion(q);

			vec3 _o = -o;
			m.r[3] = XMVector4Transform(vec4(_o.x, _o.y, _o.z, 1), m);
			vec3 d = o + t;
			m.r[3] = XMVectorAdd(m.r[3], vec4(d.x, d.y, d.z, 0));
			return m;
		}

		// [-PI..PI) に正規化
		inline float WrapPI(float x) {
			constexpr float TAU = 2 * PI;
			x = std::fmod(x + PI, TAU);
			if (x < 0) x += TAU;
			return x - PI;
		}

		//xが[lo..hi]の範囲に含まれるか？
		inline bool InInterval(float x, float lo, float hi) {
			// lo..hi がラップしているかどうかで内包判定
			if (lo == hi) return x == lo;             // 退化: 1点
			if (hi >= lo) return (x >= lo && x <= hi); // 非ラップ
			return (x >= lo || x <= hi);               // ラップ
		}

		// 円上の角度a,b[rad]の最短差の絶対値
		inline float ShortestArc(float a, float b) {
			float d = WrapPI(a - b); // (-PI, PI]
			return std::fabs(d);
		}

		//[-PI..PI)の範囲で記述された[mi..ma]の範囲にaを入れる
		// firstにはクランプ後のaの値。secondにはaから範囲までの距離(0で範囲内)
		inline std::pair<float, float> ClampAngle(float a, float mi, float ma) {
			// 1) すべて [-PI, PI) に正規化
			a = WrapPI(a);
			mi = WrapPI(mi);
			ma = WrapPI(ma);

			// 2) 退化: 区間が1点
			if (mi == ma) {
				return { mi, ShortestArc(a, mi) };
			}

			// 3) 中にいればそのまま
			if (InInterval(a, mi, ma)) {
				return { a, 0.0f };
			}

			// 4) 外なら近い端点へ（円上の最短距離で比較）
			float dmi = ShortestArc(a, mi);
			float dma = ShortestArc(a, ma);
			if (dmi < dma)
				return { mi, dmi };
			else
				return { ma, dma };
		}

		//クォータニオンの回転軸と回転角度を返す。qは正規化済みとする
		inline std::pair<vec3, float> AxisAngle(const vecQ& q)
		{
			if (abs(q.w) > 0.999999f) {
				//回転量のないクォータニオンの場合、軸は定義できないので0ベクトルを返す
				return { {0, 0, 0}, 0 };
			}

			//acosfよりatan2fを使った方が角度が小さい場合に高精度との事
			float vnorm = Length(xyz(q));
			vec3 axis = Normalize(xyz(q));
			float a = WrapPI(2 * atan2f(vnorm, q.w));
			if (a < 0) {
				a = -a;
				axis = -axis;
			}

			return { axis, a };
		}

		//qのうちa周りの回転だけ抽出(twist成分)
		//a周り以外の回転 = swing成分が欲しい場合はq * Conjugate(twist)
		//qとaは正規化済みである事
		inline vecQ ExtractTwist(const vecQ& q, const vec3& a)
		{
			// qのベクトル部をa方向に射影
			vec3 v_proj = a * Dot(a, xyz(q));
			// twist quaternion
			vecQ twist = Normalize({ v_proj.x, v_proj.y, v_proj.z, q.w });

			// 符号を揃えて連続性を保つ
			if (Dot(twist, q) < 0) twist = -twist;
			return twist;
		}

		//軸a周りに投影した場合に何度回転するクォータニオンなのか返す
		//qとaは正規化済みである事
		inline float AngleAroundAxis(const vecQ& q, const vec3& a)
		{
			return WrapPI(2.0f * atan2f(Dot(a, xyz(q)), q.w));
		}

		//移植元
		// IbukiHash by Andante (https://twitter.com/andanteyk)
		float ibuki(DWORD v)
		{
			const DWORD mult[4] = {0xae3cc725, 0x9fe72885, 0xae36bfb5, 0x82c1fcad};

			DWORD u[4] = {v,v,v,v};

			for (int i=0; i<4; i++)
				u[i] = u[i] * mult[i];

			const int p[4] = { 3,0,1,2 };
			for (int i=0; i<4; i++)
				u[i] ^= u[p[i]] ^ u[i] >> 13;

			DWORD r = 0;
			for (int i=0; i<4; i++)
				r += u[i]*mult[i];

			r ^= r >> 11;
			r = (r * r) ^ r;

			return (float)(r * 2.3283064365386962890625e-10);
		}

		//周波数1のバリューノイズを得る
		float Noise(float t)
		{
			float fr = floorf(t);
			float w = t - fr;
			float u = w * w * w * (w * (w * 6.0f - 15.0f) + 10.0f);
			DWORD t0 = (DWORD)fr;
			float n0 = ibuki(t0);
			float n1 = ibuki(t0 + 1);
			return n0 * (1 - u) + n1 * u;
		}

		//重力ノイズのためのfBm、最大8オクターブ使えるようにしたけど揺れがよくわからなくなるので1オクターブにした
		float FBM(float t, float freq)
		{
			const int octaves = 1;
			float phase[8] = { 0.715851f, 0.93483f , 0.390624f , 0.40324f , 0.0814189f , 0.00160588f , 0.227414f, 0.665702f };
			float a = 1;
			float ta = 0;
			float f = freq;
			float s = 0;
			for (int i = 0; i < octaves; i++) {
				s += Noise(t * f + phase[i]);
				ta += a;
				f *= 2;
				a *= 0.5;
			}
			return s / ta;
		}
	}
}



template <class T> void FindFrame(std::map<int, T>& keys, int target, T& t0, T& t1, bool& same)
{
	int nKeys = keys.size();

	if ((target < 0) || (nKeys <= 1)) {
		t1 = t0 = keys[0];
		same = true;
		return;
	}

	auto it = keys.upper_bound(target);
	if (it == keys.end()) {
		t1 = t0 = keys.crbegin()->second;
		same = true;
		return;
	} else if (it == keys.begin()) {
		t1 = t0 = it->second;
		same = true;
		return;
	}

	t1 = it->second;
	it--;
	t0 = it->second;
	same = false;

}


module : private;

#pragma warning(disable: 4267 4244 4305)

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace std;
namespace fs = std::filesystem;
using namespace PMX::Math;

	/***********************************************************************************/
	//デバッグとかファイル操作とか文字列ユーティリティ
	/***********************************************************************************/
namespace {
	string wstrTostr(wstring w, UINT codepage = CP_ACP) {
		int iBufferSize = WideCharToMultiByte(codepage, 0, w.c_str(), -1, (char*)NULL, 0, NULL, NULL);
		CHAR* cpMultiByte = new CHAR[iBufferSize];
		WideCharToMultiByte(codepage, 0, w.c_str(), -1, cpMultiByte, iBufferSize, NULL, NULL);
		std::string oRet(cpMultiByte, cpMultiByte + iBufferSize - 1);
		delete[] cpMultiByte;
		return(oRet);
	}

	wstring Format(wstring format, ...) {
		wchar_t strbuf[4096];
		va_list args;
		va_start(args, format);
		vswprintf_s(strbuf, 4096, format.c_str(), args);
		va_end(args);
		return strbuf;
	}

	void LOG(wstring format, ...) {
//#ifdef _DEBUG
		wchar_t strbuf[4096];
		va_list args;
		va_start(args, format);
		vswprintf_s(strbuf, 4096, format.c_str(), args);
		va_end(args);
		OutputDebugStringW(strbuf);
		OutputDebugStringW(L"\n");
//#endif
	}

	HRESULT ThrowIfFailed(HRESULT hr, wstring format, ...) {
		if (hr != S_OK) {
			wchar_t strbuf[4096];
			va_list args;
			va_start(args, format);
			vswprintf_s(strbuf, 4096, format.c_str(), args);
			va_end(args);
			wstring err = strbuf;
			err += Format(L" HRESULT=%x", hr);
			OutputDebugStringW(err.c_str());
			OutputDebugStringA(std::system_category().message(hr).c_str());
			OutputDebugStringA("\n");
			throw PMX::PMXException(wstrTostr(err).c_str());
		}
		return hr;
	}

	wstring strTowstr(string s, UINT codepage) {
		int iBufferSize = MultiByteToWideChar(codepage, 0, s.c_str(), -1, (wchar_t*)NULL, 0);
		wchar_t* cpUCS2 = new wchar_t[iBufferSize];
		MultiByteToWideChar(codepage, 0, s.c_str(), -1, cpUCS2, iBufferSize);
		std::wstring oRet(cpUCS2, cpUCS2 + iBufferSize - 1);
		delete[] cpUCS2;
		return(oRet);
	}

	wstring vec2wstr(Vector3 v) { return Format(L"%.3f,%.3f,%.3f", v.x, v.y, v.z); }
	wstring vec2wstr(Vector4 v) { return Format(L"%.3f,%.3f,%.3f,%.3f", v.x, v.y, v.z, v.w); }
	wstring quat2wstr(Vector4 v) { return Format(L"%.3f,%.3f,%.3f,%.3f", v.x, v.y, v.z, v.w); }
	wstring mat2wstr(const Matrix& m) { return Format(L"[%.3f,%.3f,%.3f,%.3f],[%.3f,%.3f,%.3f,%.3f],[%.3f,%.3f,%.3f,%.3f],[%.3f,%.3f,%.3f,%.3f]",
		m._11, m._12, m._13, m._14,  m._21, m._22, m._23, m._24,  m._31, m._32, m._33, m._34,  m._41, m._42, m._43, m._44); }


	void PrintBoneStat(PMX::PMXModel* pmx, vector<PMX::PoseBone*>& bones, const wstring& name, bool parents = false) {
		bool found = false;
		for (int i = 0; i < pmx->bones.size(); i++) {
			if (pmx->bones[i].name == name) {
				found = true;
				auto b = bones[i];
				LOG(L"%.3d %s : o:%s q:%s t:%s", b->iBone, pmx->bones[b->iBone].name, vec2wstr(b->origin).c_str(), quat2wstr(b->localRotation).c_str(), vec2wstr(b->localTranslation).c_str());
				if (parents) {
					while (b = b->parent) {
						if (b->iBone >= 0)
							LOG(L"%.3d %s : o:%s q:%s t:%s", b->iBone, pmx->bones[b->iBone].name, vec2wstr(b->origin).c_str(), quat2wstr(b->localRotation).c_str(), vec2wstr(b->localTranslation).c_str());
					}
				}
			}
		}
		if (!found)
			LOG(L"not found bone %s", name.c_str());
	}

	template <class T> T FRead(FILE* fp, int count = 1)
	{
		T dest = {};
		fread_s(&dest, sizeof(T), sizeof(T), count, fp);
		return dest;
	}

	int FReadBySize(FILE* fp, int size)
	{
		if (size == 1)
			return FRead<char>(fp);
		else if (size == 2)
			return FRead<short>(fp);
		else if (size == 4)
			return FRead<int>(fp);
		else
			return 0;
	}

	//頂点だけの特別版
	int FReadBySizeVertex(FILE* fp, int size)
	{
		if (size == 1)
			return FRead<::byte>(fp);
		else if (size == 2)
			return FRead<unsigned short>(fp);
		else if (size == 4)
			return FRead<int>(fp);	//4バイトの時だけ符号あり
		else
			return 0;
	}
}

namespace PMX {
	/***********************************************************************************/
	//PMX
	/***********************************************************************************/
	wstring PMXModel::FReadString(FILE* fp) const
	{
		int count = FRead<int>(fp);

		if (m_encoding == PMXEncoding::utf8) {
			char* cbuf = new char[count + 1];
			fread_s(cbuf, count, count, 1, fp);
			cbuf[count] = 0;
			int size = MultiByteToWideChar(CP_UTF8, 0, cbuf, -1, nullptr, 0);
			wchar_t* wbuf = new wchar_t[size];
			MultiByteToWideChar(CP_UTF8, 0, cbuf, -1, wbuf, size);
			delete[] cbuf;
			wstring ret = wbuf;
			delete[] wbuf;

			return ret;
		} else {
			wchar_t* buf = new wchar_t[(count / 2) + 1];
			fread_s(buf, count, count, 1, fp);
			buf[count / 2] = 0;
			wstring ret = buf;
			delete[] buf;
			return ret;
		}
	}

	PMXVertex PMXModel::FReadVertex(FILE* fp)
	{
		PMXVertex v = {};
		v.position = FRead<XMFLOAT3>(fp);
		v.normal = FRead<XMFLOAT3>(fp);

		//UVと追加UV
		v.uv = FRead<XMFLOAT2>(fp);
		for (int i = 0; i < m_extra_uv; i++) {
			v.extra_uv[i] = FRead<XMFLOAT4>(fp);
		}
		for (int i = m_extra_uv; i < 4; i++) {
			v.extra_uv[i] = vec4(0, 0, 0, 0);
		}

		v.weightType = FRead<char>(fp);

		if (v.weightType == 0) {
			//BDEF1
			v.bone[0] = FReadBySize(fp, m_size_bi);
			v.bone[1] = -1;
			v.bone[2] = -1;
			v.bone[3] = -1;
			v.weight[0] = 1;
			v.weight[1] = 0;
			v.weight[2] = 0;
			v.weight[3] = 0;
		} else if (v.weightType == 1) {
			//BDEF2
			v.bone[0] = FReadBySize(fp, m_size_bi);
			v.bone[1] = FReadBySize(fp, m_size_bi);
			v.bone[2] = -1;
			v.bone[3] = -1;
			v.weight[0] = FRead<float>(fp);
			v.weight[1] = 1 - v.weight[0];
			v.weight[2] = 0;
			v.weight[3] = 0;
		} else if (v.weightType == 2) {
			//BDEF4
			for (int i = 0; i < 4; i++) v.bone[i] = FReadBySize(fp, m_size_bi);
			for (int i = 0; i < 4; i++) v.weight[i] = FRead<float>(fp);
		} else if (v.weightType == 3) {
			//SDEF
			v.bone[0] = FReadBySize(fp, m_size_bi);
			v.bone[1] = FReadBySize(fp, m_size_bi);
			v.weight[0] = FRead<float>(fp);
			v.weight[1] = 1 - v.weight[0];
			v.sdef_c = FRead<XMFLOAT3>(fp);
			v.sdef_r0 = FRead<XMFLOAT3>(fp);
			v.sdef_r1 = FRead<XMFLOAT3>(fp);
			v.bone[2] = -1;
			v.bone[3] = -1;
			v.weight[2] = 0;
			v.weight[3] = 0;
		}
		v.edge = FRead<float>(fp);

		return v;
	}

	PMXMaterial PMXModel::FReadMaterial(FILE* fp)
	{
		PMXMaterial m;
		m.name = FReadString(fp);
		m.name_e = FReadString(fp);

		m.diffuse = FRead<XMFLOAT4>(fp);
		m.specular = FRead<XMFLOAT3>(fp);
		m.shininess = FRead<float>(fp);
		m.ambient = FRead<XMFLOAT3>(fp);
		m.drawFlag = FRead<char>(fp);
		m.edgeColor = FRead<XMFLOAT4>(fp);
		m.edgeSize = FRead<float>(fp);
		m.tex = FReadBySize(fp, m_size_ti);
		m.spTex = FReadBySize(fp, m_size_ti);
		m.spmode = FRead<char>(fp);
		m.toonFlag = FRead<char>(fp);
		if (m.toonFlag == 0)
			m.toonTex = FReadBySize(fp, m_size_ti);
		else
			m.toonTex = FReadBySize(fp, 1);

		wstring dmy = FReadString(fp);	//メモ、読み飛ばす

		m.vertexCount = FRead<int>(fp);

		return m;
	}

	PMXBone PMXModel::FReadBone(FILE* fp)
	{
		PMXBone b;

		b.name = FReadString(fp);
		b.name_e = FReadString(fp);

		b.position = FRead<XMFLOAT3>(fp);
		b.parent = FReadBySize(fp, m_size_bi);
		b.level = FRead<int>(fp);
		b.flag = FRead<unsigned short>(fp);

		if ((b.flag & 0x0001) == 0) {
			//接続先はオフセット指定
			b.toOffset = FRead<XMFLOAT3>(fp);
			b.toBone = -1;
		} else {
			//接続先はボーン指定
			b.toOffset = vec3(0, 0, 0);
			b.toBone = FReadBySize(fp, m_size_bi);
		}

		//付与
		if (b.IsAppendRotation() || b.IsAppendTranslation()) {
			b.appendParent = FReadBySize(fp, m_size_bi);
			b.appendRatio = FRead<float>(fp);
		}

		//軸固定
		if (b.IsFixAxis())
			b.fixAxis = FRead<XMFLOAT3>(fp);
		//ローカル軸
		if (b.IsLocalFrame()) {
			b.localAxisX = FRead<XMFLOAT3>(fp);
			b.localAxisZ = FRead<XMFLOAT3>(fp);
		}
		//外部親変形
		if (b.IsExternal()) {
			b.externalKey = FRead<int>(fp);
		}

		//IK
		if (b.IsIK()) {
			b.IKTarget = FReadBySize(fp, m_size_bi);
			b.IKLoopCount = FRead<int>(fp);
			b.IKAngle = FRead<float>(fp);
			b.IKLinkCount = FRead<int>(fp);
			b.IKLinks = vector<PMXIKLink>(b.IKLinkCount);
			for (int i = 0; i < b.IKLinkCount; i++) {
				b.IKLinks[i].bone = FReadBySize(fp, m_size_bi);
				b.IKLinks[i].isLimit = FRead<char>(fp);
				if (b.IKLinks[i].isLimit) {
					b.IKLinks[i].low = FRead<XMFLOAT3>(fp);
					b.IKLinks[i].hi = FRead<XMFLOAT3>(fp);
				}
				if (b.IKLinks[i].bone >= 0) {
					b.m_isIKLink = true;
				}
			}
		}

		return b;
	}

	PMXMorph PMXModel::FReadMorph(FILE* fp)
	{
		PMXMorph mo = {};

		mo.name = FReadString(fp);
		mo.name_e = FReadString(fp);

		mo.panel = (int)FRead<char>(fp);	//操作パネル
		mo.kind = FRead<char>(fp);
		mo.offsetCount = FRead<int>(fp);
		if (mo.kind == 0) {
			mo.offsets.resize(sizeof(PMXGroupMorphOffset) * mo.offsetCount);
			auto mof = (PMXGroupMorphOffset*)mo.offsets.data();
			//mo.offsets = new PMXGroupMorphOffset[mo.offsetCount];
			//auto mof = (PMXGroupMorphOffset*)mo.offsets;
			for (int i = 0; i < mo.offsetCount; i++) {
				mof->morph = FReadBySize(fp, m_size_moi);
				mof->ratio = FRead<float>(fp);
				mof++;
			}
		} else if (mo.kind == 1) {
			mo.offsets.resize(sizeof(PMXVertexMorphOffset) * mo.offsetCount);
			auto mof = (PMXVertexMorphOffset*)mo.offsets.data();
			//mo.offsets = new PMXVertexMorphOffset[mo.offsetCount];
			//auto mof = (PMXVertexMorphOffset*)mo.offsets;
			for (int i = 0; i < mo.offsetCount; i++) {
				mof->vertex = FReadBySizeVertex(fp, m_size_vi);
				mof->offset = FRead<XMFLOAT3>(fp);
				mof++;
			}
		} else if (mo.kind == 2) {
			mo.offsets.resize(sizeof(PMXBoneMorphOffset) * mo.offsetCount);
			auto mof = (PMXBoneMorphOffset*)mo.offsets.data();
			//mo.offsets = new PMXBoneMorphOffset[mo.offsetCount];
			//auto mof = (PMXBoneMorphOffset*)mo.offsets;
			for (int i = 0; i < mo.offsetCount; i++) {
				mof->bone = FReadBySize(fp, m_size_bi);
				mof->translation = FRead<XMFLOAT3>(fp);
				mof->rotation = FRead<XMFLOAT4>(fp);
				mof++;
			}
		} else if ((mo.kind >= 3) && (mo.kind <= 7)) {
			mo.offsets.resize(sizeof(PMXUVMorphOffset) * mo.offsetCount);
			auto mof = (PMXUVMorphOffset*)mo.offsets.data();
			//mo.offsets = new PMXUVMorphOffset[mo.offsetCount];
			//auto mof = (PMXUVMorphOffset*)mo.offsets;
			for (int i = 0; i < mo.offsetCount; i++) {
				mof->iuv = mo.kind - 3;
				mof->vertex = FReadBySizeVertex(fp, m_size_vi);
				mof->offset = FRead<XMFLOAT4>(fp);
				mof++;
			}
		} else if (mo.kind == 8) {
			mo.offsets.resize(sizeof(PMXMaterialMorphOffset) * mo.offsetCount);
			auto mof = (PMXMaterialMorphOffset*)mo.offsets.data();
			//mo.offsets = new PMXMaterialMorphOffset[mo.offsetCount];
			//auto mof = (PMXMaterialMorphOffset*)mo.offsets;
			for (int i = 0; i < mo.offsetCount; i++) {
				mof->material = FReadBySize(fp, m_size_mi);
				mof->op = FRead<char>(fp);
				mof->diffuse = FRead<XMFLOAT4>(fp);
				mof->specular = FRead<XMFLOAT3>(fp);
				mof->shininess = FRead<float>(fp);
				mof->ambient = FRead<XMFLOAT3>(fp);
				mof->edgeColor = FRead<XMFLOAT4>(fp);
				mof->edgeSize = FRead<float>(fp);
				mof->tex = FRead<XMFLOAT4>(fp);
				mof->spTex = FRead<XMFLOAT4>(fp);
				mof->toon = FRead<XMFLOAT4>(fp);
				mof++;
			}
		} else if (mo.kind == 9) {
			mo.offsets.resize(sizeof(PMXFlipMorphOffset) * mo.offsetCount);
			auto mof = (PMXFlipMorphOffset*)mo.offsets.data();
			for (int i = 0; i < mo.offsetCount; i++) {
				mof->morph = FReadBySize(fp, m_size_moi);
				mof->value = FRead<float>(fp);
				mof++;
			}
		} else if (mo.kind == 10) {
			mo.offsets.resize(sizeof(PMXImpulseMorphOffset) * mo.offsetCount);
			auto mof = (PMXImpulseMorphOffset*)mo.offsets.data();
			for (int i = 0; i < mo.offsetCount; i++) {
				mof->body = FReadBySize(fp, m_size_ri);
				mof->isLocal = FRead<BYTE>(fp);
				mof->linear = FRead<XMFLOAT3>(fp);
				mof->angular = FRead<XMFLOAT3>(fp);
				mof++;
			}
		}

		return mo;
	}

	PMXNodeItem PMXModel::FReadNodeItem(FILE* fp)
	{
		PMXNodeItem ni;

		char flag = FRead<char>(fp);
		if (flag == 0) {
			ni.isBone = true;
			ni.isMorph = false;
			ni.index = FReadBySize(fp, m_size_bi);
		} else {
			ni.isBone = false;
			ni.isMorph = true;
			ni.index = FReadBySize(fp, m_size_moi);
		}

		return ni;
	}

	PMXNode PMXModel::FReadNode(FILE* fp)
	{
		PMXNode n;

		n.name = FReadString(fp);
		n.name_e = FReadString(fp);

		//LOG(L"node:%s", n.name);
		n.flag = FRead<char>(fp);
		n.items.resize(FRead<int>(fp));
		for (int i = 0; i < n.items.size(); i++) {
			n.items[i] = FReadNodeItem(fp);
			//LOG(L"index:%d", n.items[i].index);
		}

		return n;
	}

	PMXBody PMXModel::FReadBody(FILE* fp)
	{
		PMXBody bd;
		bd.name = FReadString(fp);
		bd.name_e = FReadString(fp);

		bd.bone = FReadBySize(fp, m_size_bi);
		bd.group = FRead<char>(fp);
		bd.passGroup = FRead<USHORT>(fp);
		bd.boxKind = FRead<char>(fp);
		bd.boxSize = FRead<XMFLOAT3>(fp);
		bd.position = FRead<XMFLOAT3>(fp);
		bd.rotation = FRead<XMFLOAT3>(fp);
		bd.mass = FRead<float>(fp);
		bd.positionDamping = FRead<float>(fp);
		bd.rotationDamping = FRead<float>(fp);
		bd.restitution = FRead<float>(fp);
		bd.friction = FRead<float>(fp);
		bd.mode = FRead<char>(fp);
		//LOG(L"%s", bd.name);

		return bd;
	}

	PMXJoint PMXModel::FReadJoint(FILE* fp)
	{
		PMXJoint j;

		j.name = FReadString(fp);
		j.name_e = FReadString(fp);

		j.kind = FRead<char>(fp);

		j.bodyA = FReadBySize(fp, m_size_ri);
		j.bodyB = FReadBySize(fp, m_size_ri);
		j.position = FRead<XMFLOAT3>(fp);
		j.rotation = FRead<XMFLOAT3>(fp);
		j.moveLo = FRead<XMFLOAT3>(fp);
		j.moveHi = FRead<XMFLOAT3>(fp);
		j.angleLo = FRead<XMFLOAT3>(fp);
		j.angleHi = FRead<XMFLOAT3>(fp);
		j.springMove = FRead<XMFLOAT3>(fp);
		j.springRotate = FRead<XMFLOAT3>(fp);

		//LOG(L"%s : %s->%s", j.name, m_rs[j.bodyA].name, m_rs[j.bodyB].name);
		return j;
	}



	PMXModel::PMXModel(const wchar_t* fname, bool headerOnly)
	{
		FILE* fp;
		_wfopen_s(&fp, fname, L"rb");
		if (!fp) {
			ThrowIfFailed(E_INVALIDARG, L"can't open PMXfile!");
		}

		//マジックナンバーの確認
		char magic[5];
		fread_s(magic, 4, 4, 1, fp);
		magic[4] = 0;
		if (strcmp(magic, "PMX ") != 0)
			ThrowIfFailed(E_INVALIDARG, L"invalid PMX file!");
		version = FRead<float>(fp);
		if (version < 2.0f || version > 2.1f)
			ThrowIfFailed(E_INVALIDARG, L"unsupproted PMX version!");
		//後続データ数の確認(8で固定とのこと)
		BYTE data = FRead<BYTE>(fp);
		if (data != 8) ThrowIfFailed(E_INVALIDARG, L"invalid PMX file!");
		//8つの「後続データ」読み込み
		data = FRead<BYTE>(fp);
		if (data != 0)
			m_encoding = PMXEncoding::utf8;

		m_extra_uv = FRead<BYTE>(fp);
		m_size_vi = FRead<BYTE>(fp);
		m_size_ti = FRead<BYTE>(fp);
		m_size_mi = FRead<BYTE>(fp);
		m_size_bi = FRead<BYTE>(fp);
		m_size_moi = FRead<BYTE>(fp);
		m_size_ri = FRead<BYTE>(fp);

		//モデル名と説明文
		name = FReadString(fp);
		name_e = FReadString(fp);
		description = FReadString(fp);
		description_e = FReadString(fp);

		//頂点情報
		int vcount = FRead<int>(fp);
		if (headerOnly) {
			//ヘッダ情報のみ必要な場合はここで帰る
			fclose(fp);
			return;
		}
		vertices = vector<PMXVertex>(vcount);
		for (int i = 0; i < vcount; i++)
			vertices[i] = FReadVertex(fp);

		//頂点インデックス情報
		int icount = FRead<int>(fp);
		indices = vector<int>(icount);
		for (int i = 0; i < icount; i++)
			indices[i] = FReadBySizeVertex(fp, m_size_vi);

		//テクスチャ情報
		int tcount = FRead<int>(fp);
		textures = vector<wstring>(tcount);
		for (int i = 0; i < tcount; i++) {
			textures[i] = FReadString(fp);
		}

		//材質情報
		int mcount = FRead<int>(fp);
		materials = vector<PMXMaterial>(mcount);
		for (int i = 0; i < mcount; i++)
			materials[i] = FReadMaterial(fp);

		//テクスチャの絶対パスの取得
		absTextures = vector<wstring>(tcount);
		for (int i = 0; i < tcount; i++) {
			auto p = fs::path(textures[i]);
			if (p.is_absolute())
				absTextures[i] = p;
			else {
				auto q = fs::path(fname);
				auto w = q.parent_path().wstring();
				absTextures[i] = w + L"\\" + textures[i];
			}
		}

		//ボーン情報
		int bcount = FRead<int>(fp);
		bones = vector<PMXBone>(bcount);
		for (int i = 0; i < bcount; i++)
			bones[i] = FReadBone(fp);

		//モーフ情報
		int mocount = FRead<int>(fp);
		morphs = vector<PMXMorph>(mocount);
		for (int i = 0; i < mocount; i++)
			morphs[i] = FReadMorph(fp);

		//表示枠情報
		int ncount = FRead<int>(fp);
		nodes = vector<PMXNode>(ncount);
		for (int i = 0; i < ncount; i++)
			nodes[i] = FReadNode(fp);

		//剛体
		int rcount = FRead<int>(fp);
		bodies = vector<PMXBody>(rcount);
		for (int i = 0; i < rcount; i++)
			bodies[i] = FReadBody(fp);

		//ジョイント
		int jcount = FRead<int>(fp);
		joints = vector<PMXJoint>(jcount);
		for (int i = 0; i < jcount; i++)
			joints[i] = FReadJoint(fp);

		fclose(fp);


		//後処理…「物理＋ボーン位置合わせ」剛体がジョイントで「ボーン追従」剛体以外を親としてつながっている場合は、「物理演算」剛体に変換する
		for (int i = 0; i < joints.size(); i++) {
			const auto& jt = joints[i];
			const auto& A = bodies[jt.bodyA];
			const auto& B = bodies[jt.bodyB];
			if (A.mode != 0 && B.mode == 2 && A.bone != -1 && B.bone != -1) {
				if (bones[B.bone].parent == A.bone) {
					//LOG(L"物理＋ボーン位置合わせ剛体 %03d, %sを物理演算剛体に変換します", jt.bodyB, B.name.c_str());
					bodies[jt.bodyB].mode = 1;
				}
			}
		}

		//物理ボーンフラグ
		for (auto&& body : bodies) {
			if (body.mode != 0 && body.bone >= 0) {
				bones[body.bone].IsPhysics = true;
				bones[body.bone].physicsMode = body.mode;
			}
		}

		/*** GPUヘルパー作成 ***/
		//スキニング情報
		skinTable = std::vector<GPUSkinning>(vcount);
		for (int i = 0;  auto && v : vertices) {
			for (int j = 0; j < 4; j++) {
				skinTable[i].iBone[j] = v.bone[j];
				skinTable [i] .weight[j] = v.weight[j];
			}
			skinTable[i].sdef_c = v.sdef_c;
			skinTable[i].weightType = v.weightType;
			DirectX::XMFLOAT3 hdR = (v.sdef_r0 - v.sdef_r1) / 2;
			if (v.weightType == 3) {
				skinTable[i].weight[1] = hdR.x;
				skinTable[i].weight[2] = hdR.y;
				skinTable[i].weight[3] = hdR.z;
			}
			i++;
		}

		//モーフ
		//まず、頂点個分だけvectorを作って各vectorに対応するモーフ列を入れる
		std::vector<std::vector<GPUMorphItem>>morphVertexTable;
		morphVertexTable.resize(vcount);
		for (int i = 0;  auto & mo : morphs) {	//ここのmoはconst auto& にしてはダメ
			if (mo.kind == 1) {
				//頂点モーフ
				auto mof = (PMX::PMXVertexMorphOffset*)mo.offsets.data();
				for (int j = 0; j < mo.offsetCount; j++) {
					GPUMorphItem mi = {};
					mi.iMorph = i;
					mi.target = -1;
					mi.delta = vec4(mof->offset.x, mof->offset.y, mof->offset.z, 0);
					morphVertexTable[mof->vertex].push_back(mi);
					mof++;
				}

			} else if (3 <= mo.kind && mo.kind <=7) {
				//UVモーフ
				auto mof = (PMX::PMXUVMorphOffset*)mo.offsets.data();
				for (int j = 0; j < mo.offsetCount; j++) {
					GPUMorphItem mi = {};
					mi.iMorph = i;
					mi.target = mo.kind - 3;
					mi.delta = mof->offset;
					morphVertexTable[mof->vertex].push_back(mi);
					mof++;
				}
			}
			i++;
		}
		//↑で作ったvectorを1つにまとめてmorphTableとす
		morphPointers.resize(vcount);
		for (int i = 0;  const auto & v : vertices) {
			//ポインタ作成
			GPUMorphPointer mp = {};
			int count = morphVertexTable[i].size();
			if (count == 0) {
				mp.where = -1;	//この頂点を動かすようなモーフが無い場合は-1を入れておく
				mp.count = 0;
				morphPointers[i] = mp;
			} else {
				mp.where = (int)morphTable.size();	//この頂点を動かすモーフについての情報は、morphTableの何番目から書かれているか？
				mp.count = count;				//この頂点を動かすモーフは何個あるか？
				morphPointers[i] = mp;
			}
			//MorphItemのコピー
			for (int j = 0; j < count; j++) {
				morphTable.push_back(morphVertexTable[i][j]);
			}
			i++;
		}
		//モーフテーブルが空だった場合、GPUにバッファを作れないのでダミーデータを1つ入れておく
		if (morphTable.empty()) {
			morphTable.push_back({});
		}

		//面番号→マテリアル番号テーブル
		int nFaces = indices.size() / 3;
		face2material.resize(nFaces);
		int base = 0;
		for (int i = 0;  const auto & m : materials) {
			int count = m.vertexCount / 3;
			for (int j = 0; j < count; j++) {
				face2material[j + base] = i;
			}
			base += count;
			i++;
		}

		fclose(fp);
	}

	auto ResolveDuplicateLambda = [&](auto& bones)->std::set<int> {
		std::set<int> ret;
		std::unordered_set<std::wstring> known;
		
		
		/* β4での方法
		   材質、材質、材質0 とあった場合、材質、材質0、材質1と元々重複していなかった材質0が材質1として書き換えられてしまう問題がある
		for (int j = 0; auto&& b : bones) {
			auto name = b.name;
			bool changed = false;
			for (int i = 0; ; i++) {
				if (known.contains(name)) {
					changed = true;
					name = b.name + std::to_wstring(i);
				} else {
					break;
				}
			}
			if (changed) {
				b.name = name;
				ret.insert(j);
			}
			known.insert(name);
			j++;
		}*/
		
		//重複後出し奴の番号をretに入れとく
		for (int i = 0; auto&& b:bones) {
			if (known.contains(b.name))
				ret.insert(i);
			else
				known.insert(b.name);
			i++;
		}

		//重複後出し奴だけリネーム
		for (int dup : ret) {
			for (int i = 0; ;i++) {
				auto name = bones[dup].name + std::to_wstring(i);
				if (!known.contains(name)) {
					bones[dup].name = name;
					known.insert(name);
					break;
				}
			}
		}

		return ret;
	};

	std::set<int> PMXModel::ResolveDuplicatedBoneNames()
	{
		return ResolveDuplicateLambda(bones);
	}

	std::set<int> PMXModel::ResolveDuplicatedMaterialNames()
	{
		return ResolveDuplicateLambda(materials);
	}


	/******************************************************************************************/
	// VMD
	/******************************************************************************************/

	VMDMotionANSI VMD::FreadMotion(FILE* fp)
	{
		VMDMotionANSI m = {};

		char boneA[16] = {};
		boneA[15] = 0;	//15バイト目まで\0が無く詰まってる名前対策
		fread_s(boneA, 15, 15, 1, fp);
		//wstring boneW = strTowstr(boneA, m_codepage);
		//m.name = boneW;
		m.name = boneA;

		//続く32バイト一気に読む(iFrame(4), position(12), rotation(16))
		fread_s((void*)&m.iFrame, 32, 32, 1, fp);

		//補間データ読み込み
		char ip[64];
		fread_s((void*)ip, 64, 64, 1, fp);

		/* 元のvmdに格納されている並び順(先頭16バイト)
		char Xax, Yax, Zax, Rax;
		char Xay, Yay, Zay, Ray;
		char Xbx, Ybx, Zbx, Rbx;
		char Xby, Yby, Zby, Rby;
		*/

		//先頭16個をfloatに変換してパラメータに入れる
		int o = 0;
		m.bezier.X[0] = ip[o + 0];
		m.bezier.X[1] = ip[o + 4];
		m.bezier.X[2] = ip[o + 8];
		m.bezier.X[3] = ip[o + 12];

		m.bezier.Y[0] = ip[o + 1];
		m.bezier.Y[1] = ip[o + 5];
		m.bezier.Y[2] = ip[o + 9];
		m.bezier.Y[3] = ip[o + 13];

		m.bezier.Z[0] = ip[o + 2 + 15];	//物理が+2に入ってるので
		m.bezier.Z[1] = ip[o + 6];
		m.bezier.Z[2] = ip[o + 10];
		m.bezier.Z[3] = ip[o + 14];

		m.bezier.R[0] = ip[o + 3 + 15];
		m.bezier.R[1] = ip[o + 7];
		m.bezier.R[2] = ip[o + 11];
		m.bezier.R[3] = ip[o + 15];

		//物理キー 黒猫目薬氏の解析により追加(2024-1204)
		if (ip[o + 2] == 0x63 && ip[o + 3] == 0x0f) {
			m.physics = false;
		}
		return m;
	}

	VMDMorphANSI VMD::FreadMorph(FILE* fp)
	{
		VMDMorphANSI mo = {};

		//モーフ名をwstrに変換
		char buf[16] = {};
		buf[15] = 0;
		fread_s(buf, 15, 15, 1, fp);
		//wstring nameW = strTowstr(buf, m_codepage);
		//mo.name = nameW;
		mo.name = buf;

		//8バイト読む(iFrame, value)
		fread_s((void*)&mo.iFrame, 8, 8, 1, fp);

		return mo;
	}

	VMDCamera VMD::FreadCamera(FILE* fp)
	{
		VMDCamera cam = {};

		cam.iFrame = FRead<int>(fp);
		cam.distance = FRead<float>(fp);
		cam.target = FRead<XMFLOAT3>(fp);
		cam.rotation = FRead<XMFLOAT3>(fp);

		//ベジェパラメータ
		char b[24];
		fread_s(b, 24, 24, 1, fp);
		for (int i = 0; i < 6; i++) {
			//もともとはax bx ay byの順だが	ax ay bx by に並び変える
			int o  = i * 4;
			cam.bezierParams[o] = b[o];
			cam.bezierParams[o+1] = b[o+2];
			cam.bezierParams[o+2] = b[o+1];
			cam.bezierParams[o+3] = b[o+3];
		}
		
		{	//視野角
			DWORD buf = FRead<DWORD>(fp);
			cam.viewAngle = (float)buf;
		}

		{	//パースフラグ(元データは0でon 1でoff)
			char buf = FRead<char>(fp);
			cam.parth = (buf == 0);
		}

		return cam;
	}

	VMDLight VMD::FreadLight(FILE* fp)
	{
		VMDLight lt = {};
		lt.iFrame = FRead<int>(fp);
		lt.color = FRead<XMFLOAT3>(fp);
		lt.direction = FRead<XMFLOAT3>(fp);
		return lt;
	}

	VMDSelfShadow VMD::FreadSelfShadow(FILE* fp)
	{
		VMDSelfShadow ss = {};
		ss.iFrame = FRead<int>(fp);
		ss.kind = (int)FRead<char>(fp);
		ss.distance = FRead<float>(fp);
		return ss;
	}

	VMDIK FreadIK(FILE* fp, UINT codepage)
	{
		VMDIK ik = {};

		char buf[21];
		fread_s(buf, 20, 20, 1, fp);
		buf[20] = 0;
		auto w = strTowstr(buf, codepage);
		//wcsncpy_s(ik.name, w.c_str(), 20);
		ik.name = w;

		ik.enabled = (FRead<char>(fp) != 0);

		return ik;
	}

	VMDExtra VMD::FreadExtra(FILE* fp)
	{
		VMDExtra si = {};

		si.iFrame = FRead<int>(fp);
		si.show = (FRead<char>(fp) != 0);
		int ikcount = FRead<int>(fp);

		si.IK.resize(ikcount);
		for (int i = 0; i < ikcount; i++) {
			si.IK[i] = FreadIK(fp, m_codepage);
		}
		return si;
	}

	template<class T>int qs_compare(void* context, const void* arg1, const void* arg2)
	{
		int f1 = ((T*)arg1)->iFrame;
		int f2 = ((T*)arg2)->iFrame;
		if (f1 > f2) return 1;
		if (f1 < f2) return -1;
		return 0;
	}

	template<class T>void sortKeysByFrame(std::vector<T>& keys)
	{
		if (keys.empty())
			return;
		qsort_s(keys.data(), keys.size(), sizeof(T), qs_compare<T>, nullptr);
	}

	VMD::VMD(const wchar_t* fname, UINT codepage, const PMX::PMXModel* target)
	{
		m_codepage = codepage;
		FILE* fp;
		_wfopen_s(&fp, fname, L"rb");
		if (!fp) {
			ThrowIfFailed(E_INVALIDARG, L"can't open VMDfile %s", fname);
			return;
		}

		//ヘッダ
		char header[30] = {};
		fread_s(header, 30, 30, 1, fp);

		if (strcmp(header, "Vocaloid Motion Data 0002") != 0) {
			ThrowIfFailed(E_INVALIDARG, L"%s is unsupported VMD file", fname);
		}

		//ボーン名、モーフ名 ANSI→unicode変換辞書
		std::unordered_map<std::string, std::wstring> bones;
		std::unordered_map<std::string, std::wstring> morphs;

		//コードページに変換した状態で最大15バイトを切り出した状態をキーとし、unicodeに変換後を値とする
		if (target) {
			for (auto&& b : target->bones)
				bones[(wstrTostr(b.name, m_codepage).substr(0, 15))] = b.name;
			for (auto&& m : target->morphs)
				morphs[(wstrTostr(m.name, m_codepage).substr(0, 15))] = m.name;
		}

		//モデル名
		char model[21] = {};		//時々20バイト一杯までモデル名が入ってて0で終わってない事があるので
		fread_s(model, 20, 20, 1, fp);
		model[20] = 0;
		auto modelW = strTowstr(model, m_codepage);
		wchar_t* tmp = new wchar_t[modelW.size() + 1];
		wcscpy_s(tmp, modelW.size() + 1, modelW.c_str());
		modelName = tmp;
		delete[] tmp;

		//モーション数
		int mcount;
		fread_s((void*)&mcount, 4, 4, 1, fp);
		motionKeys = vector<VMDMotion>(mcount);

		//モーションデータ読み込み
		for (int i = 0; i < mcount; i++) {
			auto keyA = FreadMotion(fp);
			motionKeys[i] = VMDMotion(keyA);	//name以外を代入
			if (target && bones.contains(keyA.name))
				motionKeys[i].name = bones[keyA.name];
			else
				motionKeys[i].name = strTowstr(keyA.name, codepage);

			lastFrame = max(lastFrame, motionKeys[i].iFrame);
		}

		//表情モーフ数
		int mocount;
		fread_s((void*)&mocount, 4, 4, 1, fp);
		morphKeys = vector<VMDMorph>(mocount);
		for (int i = 0; i < mocount; i++) {
			auto keyA = FreadMorph(fp);
			morphKeys[i] = VMDMorph(keyA);
			if (target && morphs.contains(keyA.name))
				morphKeys[i].name = morphs[keyA.name];
			else
				morphKeys[i].name = strTowstr(keyA.name, codepage);

			lastFrame = max(lastFrame, morphKeys[i].iFrame);
		}

		//カメラデータ
		int camcount = FRead<int>(fp);
		cameraKeys.resize(camcount);
		for (int i = 0; i < camcount; i++) {
			cameraKeys[i] = FreadCamera(fp);
			lastFrame = max(lastFrame, cameraKeys[i].iFrame);
		}

		//照明データ
		int lightcount = FRead<int>(fp);
		lightKeys.resize(lightcount);
		for (int i = 0; i < lightcount; i++) {
			lightKeys[i] = FreadLight(fp);
			lastFrame = max(lastFrame, lightKeys[i].iFrame);
		}

		//フレーム番号順にソートする
		sortKeysByFrame<VMDMotion>(motionKeys);
		sortKeysByFrame<VMDMorph>(morphKeys);
		sortKeysByFrame<VMDCamera>(cameraKeys);
		sortKeysByFrame<VMDLight>(lightKeys);

		//以下はバージョンによって「無いかもしれない」データ
		

		//セルフシャドウ
		if (feof(fp)) {
			fclose(fp);
			return;
		}
		int sscount = FRead<int>(fp);
		selfShadowKeys.resize(sscount);
		for (int i = 0; i < sscount; i++) {
			selfShadowKeys[i] = FreadSelfShadow(fp);
			lastFrame = max(lastFrame, selfShadowKeys[i].iFrame);
		}
		sortKeysByFrame<VMDSelfShadow>(selfShadowKeys);

		//表示・IK
		if (feof(fp)) {
			fclose(fp);
			return;
		}
		int sicount = FRead<int>(fp);
		extraKeys.resize(sicount);
		for (int i = 0; i < sicount; i++) {
			extraKeys[i] = FreadExtra(fp);
			lastFrame = max(lastFrame, extraKeys[i].iFrame);
		}
		sortKeysByFrame<VMDExtra>(extraKeys);


		fclose(fp);
	}

	VMD::~VMD()
	{
	}

	bool VMD::IsCameraVMDFile(const wchar_t* filename)
	{
		FILE* fp;
		_wfopen_s(&fp, filename, L"rb");
		if (!fp) {
			ThrowIfFailed(E_INVALIDARG, L"can't open VMDfile %s", filename);
			return false;
		}

		//ヘッダ
		char header[30] = {};
		fread_s(header, 30, 30, 1, fp);
		if (strcmp(header, "Vocaloid Motion Data 0002") != 0) {
			fclose(fp);
			ThrowIfFailed(E_INVALIDARG, L"%s is unsupported VMD file", filename);
			return false;
		}

		//モデル名
		fseek(fp,20,SEEK_CUR);

		//モーション数
		int mcount;
		fread_s((void*)&mcount, 4, 4, 1, fp);

		//モーションデータ読み込み
		fseek(fp, 111*mcount, SEEK_CUR);

		//表情モーフ数
		int mocount;
		fread_s((void*)&mocount, 4, 4, 1, fp);
		fseek(fp, 23*mcount, SEEK_CUR);

		//カメラデータ
		int camcount = FRead<int>(fp);
		fclose(fp);

		return (mcount==0) && (mocount==0) && (camcount > 0);
	}



	//VMD書き出し用ヘルパー関数・構造体の定義
#pragma pack(push, 1)

	//文字バッファへの変換
	void ConvertStr(char* dest, UINT destsize, const std::wstring& str, UINT codepage)
	{
		std::string s = wstrTostr(str, codepage);
		std::fill(dest, dest + destsize, 0);
		memcpy_s(dest, destsize, s.c_str(), min(destsize, s.size() + 1));
	}

	struct SerializedVMDMotion {
		char name[15];
		uint32_t frame;
		vec3 position;
		vecQ rotation;
		char bezier[64];
		SerializedVMDMotion() {};
		SerializedVMDMotion(const VMDMotion& item, UINT codepage) {
			ConvertStr(name, sizeof(name), item.name, codepage);
			frame = item.iFrame;
			position = item.position;
			rotation = item.rotation;

			char* ip = bezier;
			ip[0] = item.bezier.X[0];
			ip[4] = item.bezier.X[1];
			ip[8] = item.bezier.X[2];
			ip[12] = item.bezier.X[3];

			ip[1] = item.bezier.Y[0];
			ip[5] = item.bezier.Y[1];
			ip[9] = item.bezier.Y[2];
			ip[13] = item.bezier.Y[3];

			ip[2] = item.bezier.Z[0];
			ip[6] = item.bezier.Z[1];
			ip[10] = item.bezier.Z[2];
			ip[14] = item.bezier.Z[3];

			ip[3] = item.bezier.R[0];
			ip[7] = item.bezier.R[1];
			ip[11] = item.bezier.R[2];
			ip[15] = item.bezier.R[3];

			//16バイトずれた位置に1要素ずつずらしてコピー
			for (int i = 1; i < 16; i++) {
				ip[i + 15] = ip[i];
			}
			ip[31] = 1;
			for (int i = 2; i < 16; i++) {
				ip[i + 30] = ip[i];
			}
			ip[46] = 1;	ip[47] = 0;
			for (int i = 3; i < 16; i++) {
				ip[i + 45] = ip[i];
			}
			ip[61] = 1; ip[62] = ip[63] = 0;
			
			//物理フラグ
			if (!item.physics) {
				ip[2] = 0x63;
				ip[3] = 0x0f;
			}
		};
	};

	struct SerializedVMDMorph {
		char name[15];
		uint32_t frame;
		float value;
		SerializedVMDMorph() {};
		SerializedVMDMorph(const VMDMorph& item, UINT codepage) {
			frame = item.iFrame;
			ConvertStr(name, 15, item.name, codepage);
			value = item.value;
		}
	};

	struct SerializedVMDCamera {
		uint32_t frame;
		float distance;
		vec3 target;
		vec3 rotation;
		char bezier[24];
		uint32_t viewAngle;
		char parth;
		SerializedVMDCamera() {};
		SerializedVMDCamera(const VMDCamera& item) {
			frame = item.iFrame;
			distance = item.distance;
			target = item.target;
			rotation = item.rotation;	//2025-1027修正。演算子が=ではなく-になっていのを修正
			for (int i = 0; i < 6; i++) {
				//item側はax ay bx byの順だがbezier側はax bx ay byなので入れ替える
				int o = i * 4;
				bezier[o] = item.bezierParams[o];
				bezier[o+1] = item.bezierParams[o+2];
				bezier[o+2] = item.bezierParams[o+1];
				bezier[o+3] = item.bezierParams[o+3];
			}

			viewAngle = item.viewAngle;
			//2025-1214修正。on/offが反転した状態で書き込まれていた
			parth = item.parth ? 0 : 1;	//VMDフォーマットではパースが0でon:透視投影、1でoff:正射影
		}
	};

	struct SerializedVMDLight {
		uint32_t frame;
		vec3 color;
		vec3 direction;
		SerializedVMDLight() {};
		SerializedVMDLight(const VMDLight& item) {
			frame = item.iFrame;
			color = item.color;
			direction = item.direction;
		}
	};

	struct SerializedVMDSelfShadow {
		uint32_t frame;
		char kind;
		float distance;
		SerializedVMDSelfShadow() {};
		SerializedVMDSelfShadow(const VMDSelfShadow& item) {
			frame = item.iFrame;
			kind = item.kind;
			distance = item.distance;
		};
	};

	struct SerializedInfoIK {
		char name[20]; // "右足ＩＫ\0"などのIKボーン名の文字列 20byte
		char on_off; // IKのon/off, 0:OFF, 1:ON
		SerializedInfoIK() {};
		SerializedInfoIK(const VMDIK& ik, UINT codepage) {
			ConvertStr(name, 20, ik.name, codepage);
			on_off = ik.enabled ? 1 : 0;
		}
	};

#pragma pack(pop)

	void VMD::Save(bool camera, const wstring& filename, UINT codepage)
	{
		std::ofstream ofs(fs::path(filename), std::ios::binary);
		if (!ofs) {
			ThrowIfFailed(E_FAIL, L"PMX::Save() failed, can't open file " + filename);
			return;
		}

		auto write = [&](const auto& o) {
			ofs.write((const char*)&o, sizeof(o));
		};

		//ヘッダ
		char header[30] = "Vocaloid Motion Data 0002";
		write(header);

		//モデル名
		unsigned char moname[20] = {
			0x83, 0x4a, 0x83, 0x81, 0x83, 0x89, 0x81, 0x45, 0x8f, 0xc6, 0x96, 0xbe,	//カメラ・照明
			0x00, 0x6F, 0x6E, 0x20, 0x44, 0x61, 0x74, 0x61 }; //\0on Data
		if (!camera) {
			ConvertStr((char*)moname, 20, modelName, codepage);
		}
		write(moname);

		//モーション
		int count = motionKeys.size();
		write(count);
		for (int i = 0; i < count; i++) {
			auto& key = motionKeys[i];
			SerializedVMDMotion sv(key, codepage);
			write(sv);
		}

		//表情
		count = morphKeys.size();
		write(count);
		for (int i = 0; i < count; i++) {
			auto& key = morphKeys[i];
			SerializedVMDMorph sv(key, codepage);
			write(sv);
		}

		//カメラ
		count = cameraKeys.size();
		write(count);
		for (int i = 0; i < count; i++) {
			auto& key = cameraKeys[i];
			SerializedVMDCamera sv(key);
			write(sv);
		}

		//照明
		count = lightKeys.size();
		write(count);
		for (int i = 0; i < count; i++) {
			auto& key = lightKeys[i];
			SerializedVMDLight sv(key);
			write(sv);
		}

		//セルフ影
		count = selfShadowKeys.size();
		write(count);
		for (int i = 0; i < count; i++) {
			auto& key = selfShadowKeys[i];
			SerializedVMDSelfShadow sv(key);
			write(sv);
		}

		//extra
		count = extraKeys.size();
		write(count);
		for (int i = 0; i < count; i++) {
			auto& item = extraKeys[i];
			write(item.iFrame);
			write((char)(item.show ? 1:0));
			write((uint32_t)item.IK.size());
			for (auto&& ik:item.IK) {
				SerializedInfoIK sik(ik, codepage);
				write(sik);
			}
		}
	}



	/******************************************************************************************/
	// PoseSolver
	/******************************************************************************************/

	PoseBone::PoseBone()
	{
		origin = XMFLOAT3(0, 0, 0);

		keyTranslation = XMFLOAT3(0, 0, 0);
		keyRotation = XMQuaternionIdentity();

		appendedRotation = XMQuaternionIdentity();
		appendedTranslation = XMFLOAT3(0, 0, 0);

		IKRotation = XMQuaternionIdentity();

		localTranslation = XMFLOAT3(0, 0, 0);
		localRotation = XMQuaternionIdentity();
		toParent = XMMatrixIdentity();

		transform = XMMatrixIdentity();
	}

	PoseBone::~PoseBone()
	{
	}


	//外部親を持っているか？
	//持っている場合、transformに外部親→ワールド変換が返る
	bool PoseBone::ExternalParent(XMMATRIX& transform)
	{
		for (auto&& ex : owner->miningExtra.exp) {
			if (ex.bone == iBone) {
				auto exbone = owner->externalSolver[ex.parentID]->bones[ex.parentBone];	//外部親ボーン
				transform = Matrix::CreateTranslation(exbone->origin - origin) * exbone->transform;	//親の回転中心と自分の回転中心の差分だけ平行移動させる
				return true;
			}
		}
		return false;
	}

	//boneのワールド変換が確定している時に呼ぶ。子孫フレームのtransformをboneの状態の変化に合わせる
	//物理でワールド変換が確定済みのボーンについては再計算しない
	void PoseBone::Propagate(PoseBone* bone)
	{

		if (!bone->transformResolved)
			ThrowIfFailed(E_FAIL, L"bone(%d) 's world transform is unresolved, can't propagate it", bone->iBone);

		for (int i = 0; i < bone->children.size(); i++) {
			auto child = bone->children[i];
			if (!child->transformResolved) {
				auto tr = bone->transform;
				XMMATRIX ex;
				if (child->ExternalParent(ex))
					tr = ex;
				child->transform = child->toParent * tr;//bone->transform;
				child->transformResolved = true;
			}
			Propagate(child);
		}
	}

	//bone自体か、その親のどこかでローカル変換のみ更新されたのでワールド変換が無効というフラグを立てる
	void PoseBone::PropagateUnresolved(PoseBone* bone)
	{
		bone->transformResolved = false;

		for (int i = 0; i < bone->children.size(); i++)
			PropagateUnresolved(bone->children[i]);
	}

	//ローカル変換だけ更新し、ワールド変換が未確定である事を子孫にも伝える
	void PoseBone::UpdateLocalTransform()
	{
		toParent = oRtMatrix(origin, localRotation, localTranslation);
		PropagateUnresolved(this);
	}


	//ほぼIK用
	//このボーンのワールド変換を得る。ワールド変換が未確定の場合は確定済みの先祖ボーンにさかのぼってワールド変換を確定する
	XMMATRIX PoseBone::ResolveTransform()
	{
		if (transformResolved)
			return transform;

		//ワールド座標が確定された先祖ボーンまで辿る
		PoseBone* LocalChain[256];	//とりあえず256個あればいいだろ的な
		int nChain = 0;
		PoseBone* pb = this;

		XMMATRIX m;
		while (true) {
			if (pb->ExternalParent(m)) {
				//ハマった話…pb→ワールドの行列がセットされていないとならない
				//最初はmだけで下の段落にパスしていたので外部親の子に設定されたボーンの姿勢が反映できていなかった
				m = pb->toParent * m;	
				break;
			}
			if (pb->transformResolved){
				m = pb->transform;
				break;
			}
			LocalChain[nChain] = pb;
			pb = pb->parent;
			nChain++;
		}

		//先祖ボーンから自分まで順にワールド変換を計算
		for (int i = nChain - 1; i >= 0; i--) {
			pb = LocalChain[i];
			m = pb->toParent * m;
			pb->transform = m;
			pb->transformResolved = true;
		}

		return m;
	}

	//PoseKeySubsetをvmdオブジェクトから作成する
	KeySubset::KeySubset(const VMD* vmd)
	{
		motions.resize(vmd->motionKeys.size());
		for (int i = 0;  auto && item : motions) {
			item = vmd->motionKeys[i];
			i++;
		}

		morphs.resize(vmd->morphKeys.size());
		for (int i = 0; auto && item:morphs) {
			item = vmd->morphKeys[i];
			i++;
		}

		extras.resize(vmd->extraKeys.size());
		for (int i = 0; auto && item:extras) {
			item = vmd->extraKeys[i];
			i++;
		}

		cameras.resize(vmd->cameraKeys.size());
		for (int i = 0; auto && item:cameras) {
			item = vmd->cameraKeys[i];
			i++;
		}

		lights.resize(vmd->lightKeys.size());
		for (int i = 0; auto && item:lights) {
			item = vmd->lightKeys[i];
			i++;
		}

		selfShadows.resize(vmd->selfShadowKeys.size());
		for (int i = 0; auto && item:selfShadows) {
			item = vmd->selfShadowKeys[i];
			i++;
		}

		//重力キーはvmdに格納されてない
	}

	//格納されている全キーの選択状態をselectに変更
	void KeySubset::SelectKeys(bool select)
	{
		for (auto && key : motions)
			key.selected = select;
		for (auto&& key : morphs)
			key.selected = select;
		for (auto&& key : extras)
			key.selected = select;
		for (auto&& key : cameras)
			key.selected = select;
		for (auto&& key : lights)
			key.selected = select;
		for (auto&& key : selfShadows)
			key.selected = select;
		for (auto& key : gravities)
			key.selected = select;
	}

	int KeySubset::FirstFrame() const
	{
		int ret = 0x7FFFFFFF;
		auto lambda = [&](const auto& keys) { if (!keys.empty()) ret = min(ret, keys.front().iFrame); };
		lambda(motions);
		lambda(morphs);
		lambda(extras);
		lambda(cameras);
		lambda(lights);
		lambda(selfShadows);
		lambda(gravities);
		return ret == 0x7FFFFFFF ? 0 : ret;
	}
	
	int KeySubset::LastFrame() const
	{
		int ret = 0;
		auto lambda = [&](const auto& keys) { if (!keys.empty()) ret = max(ret, keys.back().iFrame); };
		lambda(motions);
		lambda(morphs);
		lambda(extras);
		lambda(cameras);
		lambda(lights);
		lambda(selfShadows);
		lambda(gravities);
		return ret;
	}

	bool KeySubset::Empty() const
	{
		if (!motions.empty())
			return false;
		if (!morphs.empty())
			return false;
		if (!extras.empty())
			return false;
		if (!cameras.empty())
			return false;
		if (!lights.empty())
			return false;
		if (!selfShadows.empty())
			return false;
		if (!gravities.empty())
			return false;

		return true;
	}

	//セットアップ
	PoseSolver::PoseSolver(Physics* _physics, const PMXModel* _pmx, const VMD* vmd)
	{
		type = SolverType::pose;
		pmx = _pmx;
		physics = _physics;

		//まずボーンの本数だけPoseBoneの配列を作る
		bones.reserve(pmx->bones.size());
		boneMatrices.resize(pmx->bones.size());

		root = new PoseBone;	//-1ボーン
		root->owner = this;
		root->transformResolved = true;

		//bonesにPMXのボーンデータを取り込む
		for (int i = 0;  const auto & b : pmx->bones) {
			PoseBone* pb = new PoseBone;
			pb->owner = this;
			pb->iBone = i;
			pb->origin = b.position;
			bones.push_back(pb);
			i++;
		}

		//bonesの親子関係の構築
		int maxLevel = 0;	//最大変形階層。後で使う
		for (int i = 0; const auto & b : pmx->bones) {
			maxLevel = max(maxLevel, b.level);
			auto parent = (b.parent == -1) ? root : bones[b.parent];
			bones[i]->parent = parent;				//親ボーンのセット
			parent->children.push_back(bones[i]);	//親ボーンの子リストに追加する
			i++;
		}

		//IKボーンだった場合、リンクボーンに自分の情報を書き込む
		for (int i = 0; auto&& b : bones) {
			const auto& pmxb = pmx->bones[i];
			if (pmxb.IsIK()) {
				for (int j = 0; j < pmxb.IKLinkCount; j++) {
					auto link = pmxb.IKLinks[j].bone;
					if (link >= 0) {
						bones[link]->IKMaster = i;
						LOG(L"IKLink bone #%d%s : master #%d%s", link, pmx->bones[link].name, i, pmxb.name);
					}
				}
			}
			i++;
		}

		//変形順序テーブルの作成
		vector<int>* orderPerLevelBP = new vector<int>[maxLevel + 1];	//変形階層ごとに入ってるボーンのリスト(物理前)
		vector<int>* orderPerLevelAP = new vector<int>[maxLevel + 1];	//変形階層ごとに入ってるボーンのリスト(物理後)
		bonesBeforePhys = 0;
		for (int i = 0; i < pmx->bones.size(); i++) {
			auto& b = pmx->bones[i];
			if (b.IsAfterPhysics()) {
				orderPerLevelAP[b.level].push_back(i);
			} else {
				orderPerLevelBP[b.level].push_back(i);
				bonesBeforePhys++;
			}
		}
		TFOrder.reserve(pmx->bones.size());
		for (int i = 0; i <= maxLevel; i++)
			for (int j = 0; j < orderPerLevelBP[i].size(); j++)
				TFOrder.push_back(orderPerLevelBP[i][j]);
		for (int i = 0; i <= maxLevel; i++)
			for (int j = 0; j < orderPerLevelAP[i].size(); j++)
				TFOrder.push_back(orderPerLevelAP[i][j]);

		/*
		LOG(L"%d bones in TFOrder", TFOrder.size());
		for (int i = 0; i < TFOrder.size(); i++) {
			LOG(L"%03d %s", TFOrder[i], pmx->m_bs[TFOrder[i]].name);
		}
		*/

		//付与構造のデバッグ表示
		vector<bool> fuyoflag(TFOrder.size(), false);	//既に付与構造に挙げられたボーンを重複表示しないためのフラグ
		for (int i = TFOrder.size()-1; i >= 0; i--) {
			wstring s = L"";
			PMXBone b = pmx->bones[i];
			bool first = true;
			if (!fuyoflag[i]) {
				set<int> chain;	//現在列挙中の付与チェーン(循環参照時に停止するためのデータ)
				bool cyclic = false;
				while (b.IsAppendRotation() || b.IsAppendTranslation()) {
					if (first) {
						fuyoflag[i] = true;
						s += Format(L"%s (%d, L%d)", b.name, i, b.level);
					}
					PMXBone pb = pmx->bones[b.appendParent];
					s += Format(L" <- %s (%d, L%d)", pb.name, b.appendParent, pb.level);
					//循環参照の検出
					if (chain.contains(b.appendParent)) {
						cyclic = true;
						break;
					}
					chain.insert(b.appendParent);
					fuyoflag[b.appendParent] = true;
					b = pb;
					first = false;
				}
				if (!s.empty()) {
					if (cyclic)
						LOG(L"付与構造(循環参照を含む) : %s", s.c_str());
					else
						LOG(L"付与構造 : %s", s.c_str());
				}
			}
		}

		delete[] orderPerLevelAP;
		delete[] orderPerLevelBP;


		//ボーン文字列→ボーン番号辞書
		boneDict.clear();
		for (int i = 0;  const auto & b : pmx->bones) {
			boneDict[b.name] = i;
			i++;
		}

		//TODO:現状では名前が15文字以上のボーン・モーフとマッチしない(vmdファイルの仕様上、元が15バイト以上の名前を格納できないので)
		//VMDに含まれるボーン名→ボーン番号の翻訳
		{
			//モーションをボーンごとに仕分ける
			motionKeys = vector<map<int,VMDMotionLite>>(pmx->bones.size());
			if (vmd != nullptr) {
				for (auto& m : vmd->motionKeys) {
					if (boneDict.contains(m.name)) {
						int idx = boneDict[m.name];
						motionKeys[idx].emplace_hint(motionKeys[idx].end(), m.iFrame, m);	//VMD読み込みの時点でフレーム順にソート済みなので
					}
				}
			}
			for (int i = 0; i < pmx->bones.size(); i++) {
				//0フレームにキーが入ってない場合は無回転・無移動のキーを入れておく
				if (!motionKeys[i].contains(0)) {
					VMDMotionLite m = {};
					motionKeys[i][0] = m;
				}
			}
		}

		//同様に、モーフ名→モーフ番号の翻訳
		{
			morphDict.clear();
			for (int i = 0; i < pmx->morphs.size(); i++) {
				PMXMorph mo = pmx->morphs[i];
				morphDict[mo.name] = i;
			}

			//モーフごとに仕分け
			int mocount = pmx->morphs.size();
			morphKeys = vector<map<int,VMDMorphLite>>(mocount);
			if (vmd != nullptr) {
				for (auto& mo : vmd->morphKeys) {
					if (morphDict.contains(mo.name)) {
						int idx = morphDict[mo.name];
						morphKeys[idx].emplace_hint(morphKeys[idx].end(), mo.iFrame, mo);
					}
				}
			}
			for (int i = 0; i < mocount; i++) {
				//0フレームにキーが無かったら無表情のパラメータ入れとく
				if (!morphKeys[i].contains(0)) {
					VMDMorphLite mo = {};
					morphKeys[i][0] = mo;
				}
			}

			//デフォルトモーフ値配列の作成
			m_morphDefaults.resize(mocount, 0);
		}

		//IKボーン番号←→全体ボーン番号テーブル
		{
			IK2Bone.clear();
			int bcount = bones.size();
			for (int i = 0; i < bcount; i++) {
				if (pmx->bones[i].IsIK()) {
					IK2Bone.push_back(i);
				}
			}

			Bone2IK.resize(bcount);
			for (int i = 0; i < bcount; i++) {
				auto ret = std::find(IK2Bone.begin(), IK2Bone.end(), i);
				if (ret == IK2Bone.end()) {
					Bone2IK[i] = -1;
				} else {
					Bone2IK[i] = ret - IK2Bone.begin();
				}
			}

			//Solve時にIK有効・無効フラグを格納するためのベクタ
			IKEnabled.clear();
			IKEnabled.resize(IK2Bone.size());

			//IKリンク
			m_IKJoints.resize(IK2Bone.size());
			for (int i = 0; i < m_IKJoints.size(); i++) {
				auto& b = pmx->bones[IK2Bone[i]];
				for (int j = 0; j < b.IKLinks.size(); j++) {
					auto l = IKLinkJoint(b.IKLinks[j]);
					l.bone = bones[b.IKLinks[j].bone];
					m_IKJoints[i].push_back(l);
				}
			}
		}

		//表示・IKキー
		{
			//デフォルトキー情報の作成
			defaultExtraKey = {};
			defaultExtraKey.IK.resize(IK2Bone.size());
			for (int i = 0; auto && ik : defaultExtraKey.IK) {
				ik.enabled = true;
				ik.name = pmx->bones[IK2Bone[i]].name;
				i++;
			}

			extraKeys.clear();
			if (vmd != nullptr) {
				for (auto && k : vmd->extraKeys) {
					VMDExtra key;
					key.iFrame = k.iFrame;
					key.IK.resize(IK2Bone.size());
					key.show = k.show;
					//IKの翻訳。まずモデルのIKボーン名リストを作り、キーが無い場合の値を入れる
					for (int i = 0; auto && ik:key.IK) {
						ik.enabled = true;	//注意：追加読み込みを作る際は、このデフォルト値は「直前のフレームの値」がよいと思う
						ik.name = pmx->bones[IK2Bone[i]].name;
						i++;
					}
					//該当するボーンがk.IKに含まれているならその値に書き換える
					for (auto && ik : k.IK) {
						if (boneDict.contains(ik.name)) {
							auto ib = boneDict[ik.name];
							int iik = Bone2IK[ib];
							if (iik >= 0) {
								key.IK[iik].enabled = ik.enabled;
								break;
							}
						}
					}
					extraKeys[k.iFrame] = key;
				}
			}


			//0フレームにキーが入ってない場合、初期値を入れる
			if (!extraKeys.contains(0))
				extraKeys[0] = defaultExtraKey;
		}

		//モーフ値ベクタ初期化
		morphValues.resize(pmx->morphs.size());

		//材質モーフ
		materialMorphAdd.resize(pmx->materials.size());
		materialMorphMul.resize(pmx->materials.size());

		materials.resize(pmx->materials.size());
		for (int i = 0; auto && m:materials) {
			m = Material(pmx->materials[i]);
			i++;
		}

		//インパルスモーフ
		impulseLinear.resize(pmx->bodies.size());
		impulseAngular.resize(pmx->bodies.size());
		impulseLinearLocal.resize(pmx->bodies.size());
		impulseAngularLocal.resize(pmx->bodies.size());
		impulseReset.resize(pmx->bodies.size());

		//物理対応
		AttachPhysics();
	}

	PoseSolver::~PoseSolver()
	{
		DetachPhysics();
		delete root;
		for (int i = 0; i < bones.size(); i++)
			delete bones[i];
	}

	//ベジェ補間係数y = B(x)を求める
	//半刻み法…収束するまでのイテレーションは多いが
	//①f'の評価が必要ない ②f'==0を踏んでも初期値を振り直す必要が無い という利点により安定した速度が得られる
	//以下の問題設定では平均的にニュートン法より良さそうに見える
	float BezierT(float x, char _ax, char _ay, char _bx, char _by)
	{
		float ax = _ax / 127.0f, bx = _bx / 127.0f, ay = _ay / 127.0f, by = _by / 127.0f;
		float eps = 1e-6;	//当初は1/255くらいの誤差は良いだろうと思ってたけど、意外と精度が求められるっぽい
		float t = x;
		float k0 = 1 + 3 * (ax - bx);
		float k1 = 3 * (bx - 2 * ax);
		float k2 = 3 * ax;
		for (int i = 0; i < 32; i++) {
			//float delta = k0 * t * t * t + k1 * t * t + k2 * t - x;
			float delta = ((k0 * t + k1) * t + k2) * t - x;
			if (abs(delta) <= eps) {
				break;
			}
			t -= delta / 2;
		}


		float r = 1 - t;
		//return t * t * t + 3 * t * t * r * by + 3 * t * r * r * ay;
		return ((t + 3 * r * by) * t + 3 * r * r * ay) * t;
	}

	//ニュートン法Ver. 収束するまでのイテレーションは少ないがf'(x)==0を踏むリスクあり
	/*
	float BezierT(float x, float ax, float ay, float bx, float by)
	{
		LARGE_INTEGER pc1;
		QueryPerformanceCounter(&pc1);

		float k0 = 1 + 3 * (ax - bx);
		float k1 = 3 * (bx - 2 * ax);
		float k2 = 3 * ax;
		auto ft = [&](float t) { return ((k0 * t + k1) * t + k2) * t - x; };
		auto fdash = [&](float t) { return t * (3 * t * k0 + 2 * k1) + 3 * ax; };

		//f'(t) = 0とならないような初期値t0をてきとうに探す
		float t = x;
		while (abs(fdash(t)) < 1e-6)
			t = (float)rand()/RAND_MAX;

		for (int i = 0; i < 32; i++) {
			float delta = ft(t) / fdash(t);
			t -= delta;
			if (abs(delta) < 1e-6) {
				//LOG(L"newton %d", i+1);
				break;
			}
		}

		LARGE_INTEGER pc2;
		QueryPerformanceCounter(&pc2);
		LOG(L"%lld", pc2.QuadPart - pc1.QuadPart);


		float r = 1 - t;
		return ((t + 3 * r * by) * t + 3 * r * r * ay) * t;
	}
	*/


	//VMDのモーションから現在のフレームのローカル変形(位置・回転)を得る。モーフや付与変形は抜きで、各ボーンのキーフレームから得られる情報のみ
	void PoseSolver::GetKeyFrame(float time, int iBone, XMFLOAT3& translation, XMVECTOR& rotation, bool& phys, bool contPhys)
	{
		int ib = iBone;

		if (ib < 0 || motionKeys[ib].empty()) {
			//無効なボーン番号だった場合や、対象ボーンについてのキーが無い場合は初期位置・姿勢を返す
			translation = vec3(0, 0, 0);
			rotation = vecQ::Identity;
			phys = true;
			return;
		}


		float f = time;
		int targetFrame = floorf(f);

		VMDMotion m0, m1;
		bool same;
		FindFrame<VMDMotionLite>(motionKeys[ib], targetFrame, m0, m1, same);

		//i1==i0の場合 補完しなくてよい
		if (same) {
			translation = m0.position;
			rotation = vecQ(m0.rotation);
			phys = m0.physics;
			return;
		}

		//物理キーの取り扱い
		if (contPhys) {
			//再生中など時間軸が連続している前提の場合、両側のand(on→offの時にキーフレーム補間にすることでワープしなくなる)
			phys = m0.physics && m1.physics;
			if ( m0.physics && !m1.physics && pmx->bones[ib].IsPhysics) {
				if (physPose.boneKeys.size() > ib) {
					//物理on→offの区間である場合
					//m0のパラメータを物理onキーが最後に打たれていたフレームの状態に置換する
					m0.rotation = physPose.boneKeys[ib].rotation;
					m0.position = physPose.boneKeys[ib].position;
				}

				//0Fから再生開始して 0F=on,10F=offなど 再生後いきなりこのパスにやってくるとphysPoseが不定になる
				//つまり、連続再生時も最初のフレームはcontPhys = falseにしなければならない
				//physPoseの割り当てすら行われていない場合に誤って呼び出したときは、せめて例外を起こさないようにチェックしている
			}
		} else {
			//編集中など不連続が前提の場合、左側だけ(見た目が直感的)
			phys = m0.physics;
		}

		//必要に応じて補完する
		float t = (float)(f - m0.iFrame) / (float)(m1.iFrame - m0.iFrame);

		if (vec3(m0.position) == vec3(m1.position)) {
			translation = m0.position;
		} else {
			float tx = BezierT(t, m1.bezier.X[0], m1.bezier.X[1], m1.bezier.X[2], m1.bezier.X[3]);
			float ty = BezierT(t, m1.bezier.Y[0], m1.bezier.Y[1], m1.bezier.Y[2], m1.bezier.Y[3]);
			float tz = BezierT(t, m1.bezier.Z[0], m1.bezier.Z[1], m1.bezier.Z[2], m1.bezier.Z[3]);
			translation = Lerp(m0.position, m1.position, vec3(tx, ty, tz));
		}

		if (vecQ(m0.rotation) == vecQ(m1.rotation)) {
			rotation = vecQ(m0.rotation);
		} else {
			float tr = BezierT(t, m1.bezier.R[0], m1.bezier.R[1], m1.bezier.R[2], m1.bezier.R[3]);
			rotation = vecQ::Slerp(m0.rotation, m1.rotation, tr);
		}


		//if (iBone == 0) LOG(L"frame:%.2d t:%f B(t):%f", iFrame, t, tx);

		//LOG(L"#%d %s : %s %s", iBone, pmx->m_bs[iBone].name, vec2wstr(*translation).c_str(), quat2wstr(*rotation).c_str());
	}


	//VMDのモーションから現在のフレームのモーフの値を得る
	float PoseSolver::GetKeyFrameMorph(float time, int iMorph)
	{
		int im = iMorph;

		if (im < 0 || morphKeys[im].empty()) {
			return 0;
		}

		float f = time;
		int targetFrame = floorf(f);
		VMDMorph m0, m1;
		bool same;
		FindFrame<VMDMorphLite>(morphKeys[im], targetFrame, m0, m1, same);

		if (same) {
			return m0.value;
		}

		//補完する
		float t = (float)(f - m0.iFrame) / (float)(m1.iFrame - m0.iFrame);
		return Lerp(m0.value, m1.value, t);
	}

	VMDExtra PoseSolver::GetKeyFrameExtra(float time)
	{
		//表示キーがない場合、「表示」という事にする
		if (extraKeys.empty())
			return {};
		
		//検索時刻から見て最後に打たれたキーを探す
		bool same;
		VMDExtra t0, t1;
		FindFrame(extraKeys, floorf(time), t0, t1, same);

		return t0;
	}


	//乗算モーフ処理用
	inline float MulMorph(float ofs, float value)
	{
		return Lerp(1.0f, ofs, value);
	}
	inline vec3 MulMorph3(vec3 ofs, float value)
	{
		return vec3( Lerp(1.0f, ofs.x, value), Lerp(1.0f, ofs.y, value), Lerp(1.0f, ofs.z, value));
	}
	inline vec4 MulMorph4(vec4 ofs, float value)
	{
		return vec4(Lerp(1.0f, ofs.x, value), Lerp(1.0f, ofs.y, value), Lerp(1.0f, ofs.z, value), Lerp(1.0f, ofs.w, value));
	}

	//ハイ、ポーズ！
	void PoseSolver::HiPose(Pose& pose, float time, bool integerTime, bool contPhys)
	{
		int iFrame = max(0, floorf(time));

		pose.boneKeys.resize(motionKeys.size());
		for (int i = 0; auto && item : motionKeys) {
			VMDMotionLite key = {};
			if (integerTime && item.contains(iFrame)) {
				key = item[iFrame];
				key.selected = false;
			} else {
				key.iFrame = iFrame;
				GetKeyFrame(time, i, key.position, reinterpret_cast<XMVECTOR&>(key.rotation), key.physics, contPhys);
			}
			pose.boneKeys[i] = key;
			i++;
		}

		pose.morphKeys.resize(morphKeys.size());
		for (int i = 0; auto && item : morphKeys) {
			VMDMorphLite key = {};
			if (integerTime && item.contains(iFrame)) {
				key = item[iFrame];
				key.selected = false;
			} else {
				key.iFrame = iFrame;
				key.value = GetKeyFrameMorph(time, i);
			}
			pose.morphKeys[i] = key;
			i++;
		}

		//表示・IK・外親extraキーには補間が無いので
		pose.extraKey = GetKeyFrameExtra(time);
	}

	//ポーズデータを元に各ボーンの姿勢やグループモーフの展開などを行う
	void PoseSolver::PoseMining(const Pose& pose)
	{
		miningExtra = pose.extraKey;
		int mocount = pmx->morphs.size();

		//キーフレーム読んで各ボーンのローカル変形を取得
		for (auto&& ib : TFOrder) {
			bones[ib]->keyTranslation = pose.boneKeys[ib].position;
			bones[ib]->keyRotation = Vector4(pose.boneKeys[ib].rotation);
		}

		//モーフのキーフレーム読む
		for (int i = 0; i < mocount; i++)
			morphValues[i] = pose.morphKeys[i].value;



		if (pmx->version >= 2.10f) {
			//フリップ変形実行ラムダ
			auto flipLambda = [&](auto& mo, float value) {
				//フリップモーフ値からオフセット内でモーフ値を得る対象を選択し、fvalに代入
				int idx = min((int)((mo.offsetCount + 1) * value) - 1, mo.offsetCount - 1);
				if (idx >= 0) {
					PMXFlipMorphOffset* mof = (PMXFlipMorphOffset*)mo.offsets.data();
					auto& ofs = *(mof + idx);
					morphValues[ofs.morph] = ofs.value;	//オフセットが示すモーフの値を、オフセットの値に上書
				}
			};
			//フリップモーフとグループモーフの展開
			//1パス目…フリップモーフの解決
			for (int i = 0; auto && mo:pmx->morphs) {
				if (mo.kind == 9) {
					//フリップモーフ実行
					flipLambda(mo, morphValues[i]);
				} else if (mo.kind == 0) {
					//グループモーフのオフセットに含まれるフリップモーフを実行
					PMXGroupMorphOffset* mof = (PMXGroupMorphOffset*)mo.offsets.data();
					for (int j = 0; j < mo.offsetCount; j++) {
						auto ofs = *(mof + j);
						if (pmx->morphs[ofs.morph].kind == 9) {
							flipLambda(pmx->morphs[ofs.morph], morphValues[i]);
						}
					}
				}
				i++;
			}
			//2パス目…グループモーフの解決
			for (int i = 0; i < mocount; i++) {
				auto& mo = pmx->morphs[i];
				if (mo.kind == 0) {
					PMXGroupMorphOffset* mof = (PMXGroupMorphOffset*)mo.offsets.data();
					for (int j = 0; j < mo.offsetCount; j++) {
						auto ofs = *(mof + j);
						if (pmx->morphs[ofs.morph].kind != 9)
							morphValues[ofs.morph] += morphValues[i] * ofs.ratio;
					}
				}
			}
		} else {
			//グループモーフ展開
			for (int i = 0; i < mocount; i++) {
				auto& mo = pmx->morphs[i];
				if (mo.kind == 0) {
					PMXGroupMorphOffset* mof = (PMXGroupMorphOffset*)mo.offsets.data();
					for (int j = 0; j < mo.offsetCount; j++) {
						auto ofs = *(mof + j);
						morphValues[ofs.morph] += morphValues[i] * ofs.ratio;
					}
				}
			}
		}

		//材質モーフ計算用バッファ初期化
		for (auto&& m : materialMorphAdd) {
			m.diffuse = { 0,0,0,0 };
			m.specular = { 0,0,0 };
			m.shininess = 0;
			m.ambient = { 0,0,0 };
			m.edgeColor = { 0,0,0,0 };
			m.edgeSize = 0;
			m.textureValue = { 0,0,0,0 };
			m.sphereValue = { 0,0,0,0 };
			m.toonTextureValue = { 0,0,0,0 };
		};

		for (auto&& m : materialMorphMul) {
			m.diffuse = { 1,1,1,1 };
			m.specular = { 1,1,1 };
			m.shininess = 1;
			m.ambient = { 1,1,1 };
			m.edgeColor = { 1,1,1,1 };
			m.edgeSize = 1;
			m.textureValue = { 1,1,1,1 };
			m.sphereValue = { 1,1,1,1 };
			m.toonTextureValue = { 1,1,1,1 };
		}

		//インパルスモーフ計算用バッファ初期化
		if (pmx->version >= 2.10f) {
			std::fill(impulseLinear.begin(), impulseLinear.end(), vec3(0, 0, 0));
			std::fill(impulseAngular.begin(), impulseAngular.end(), vec3(0, 0, 0));
			std::fill(impulseLinearLocal.begin(), impulseLinearLocal.end(), vec3(0, 0, 0));
			std::fill(impulseAngularLocal.begin(), impulseAngularLocal.end(), vec3(0, 0, 0));
			std::fill(impulseReset.begin(), impulseReset.end(), false);
		}


		//各モーフの処理
		bool matmorph = false;	//材質モーフが1つでも有効か？
		for (int i = 0; i < mocount; i++) {
			auto value = morphValues[i];
			if (value != 0) {
				auto& mo = pmx->morphs[i];
				if (mo.kind == 2) {
					//ボーンモーフなら各ボーンに反映する
					auto ofs = (PMXBoneMorphOffset*)mo.offsets.data();
					for (int j = 0; j < mo.offsetCount; j++) {
						int ib = ofs->bone;
						bones[ib]->keyTranslation = bones[ib]->keyTranslation + ofs->translation * value;
						bones[ib]->keyRotation = vecQ(bones[ib]->keyRotation) * vecQ::Slerp(vecQ::Identity, vecQ(ofs->rotation), value);
						ofs++;
					}
				} else if (mo.kind == 8) {
					matmorph = true;
					//材質モーフ
					auto ofs = (PMXMaterialMorphOffset*)mo.offsets.data();
					for (int j = 0; j < mo.offsetCount; j++) {
						int im = ofs->material;
						//im == -1の時は「全材質」
						int k0 = (im >= 0) ? im : 0;
						int k1 = (im >= 0) ? im : pmx->materials.size()-1;
						if (ofs->op == 0) {
							//乗算モーフ
							for (int k = k0; k <= k1; k++) {
								auto& m = materialMorphMul[k];
								m.diffuse = vec4(m.diffuse) * MulMorph4(ofs->diffuse, value);
								m.specular = m.specular * MulMorph3(ofs->specular, value);
								m.shininess = m.shininess * MulMorph(ofs->shininess, value);
								m.ambient = m.ambient * MulMorph3(ofs->ambient, value);
								m.edgeColor = vec4(m.edgeColor) * MulMorph4(ofs->edgeColor, value);
								m.edgeSize = m.edgeSize * MulMorph(ofs->edgeSize, value);
								m.textureValue = vec4(m.textureValue) * MulMorph4(ofs->tex, value);
								m.sphereValue = vec4(m.sphereValue) * MulMorph4(ofs->spTex, value);
								m.toonTextureValue = vec4(m.toonTextureValue) * MulMorph4(ofs->toon, value);
							}
						} else if (ofs->op == 1) {
							for (int k = k0; k <= k1; k++) {
								//加算モーフ
								auto& m = materialMorphAdd[k];
								m.diffuse = vec4(m.diffuse) + vec4(ofs->diffuse) * value;
								m.specular = m.specular + ofs->specular * value;
								m.shininess = m.shininess + ofs->shininess * value;
								m.ambient = m.ambient + ofs->ambient * value;
								m.edgeColor = vec4(m.edgeColor) + vec4(ofs->edgeColor) * value;
								m.edgeSize = m.edgeSize + ofs->edgeSize * value;
								m.textureValue = vec4(m.textureValue) + vec4(ofs->tex) * value;
								m.sphereValue = vec4(m.sphereValue) + vec4(ofs->spTex) * value;
								m.toonTextureValue = vec4(m.toonTextureValue) + vec4(ofs->toon) * value;
							}
						}
						ofs++;
					}
				} else if (mo.kind == 10) {
					//インパルスモーフ
					auto ofs = (PMXImpulseMorphOffset*)mo.offsets.data();
					auto is0Lambda = [](const vec3& v) { return (v.x == 0 && v.y == 0 && v.z == 0); };	//0ベクトルか判定するラムダ
					for (int j = 0; j < mo.offsetCount; j++) {
						int iRB = ofs->body;
						if (iRB >= 0) {
							if (is0Lambda(ofs->angular) && is0Lambda(ofs->linear)) {
								//初期化
								impulseLinear[iRB] = {0,0,0};
								impulseAngular[iRB] = { 0,0,0 };
								impulseReset[iRB] = true;
							} else {
								if (ofs->isLocal) {
									impulseLinearLocal[iRB] = impulseLinearLocal[iRB] + ofs->linear * value;
									impulseAngularLocal[iRB] = impulseAngularLocal[iRB] + ofs->angular * value;
								} else {
									impulseLinear[iRB] = impulseLinear[iRB] + ofs->linear * value;
									impulseAngular[iRB] = impulseAngular[iRB] + ofs->angular * value;
								}
							}
						}
						ofs++;
					}
				}
				//LOG(L"morph %.3d %s %.3f", i, mo.name, value);
			}
		}

		//材質モーフ適用後の材質のセット
		if (matmorph) {
			for (int i = 0; auto && m:materials) {
				auto& add = materialMorphAdd[i];
				auto& mul = materialMorphMul[i];
				auto& p = pmx->materials[i];
				m.diffuse = (vec4)p.diffuse * mul.diffuse + add.diffuse;
				m.specular = p.specular * mul.specular + add.specular;
				m.shininess = p.shininess * mul.shininess + add.shininess;
				m.ambient = p.ambient * mul.ambient + add.ambient;
				m.edgeColor = (vec4)p.edgeColor * mul.edgeColor + add.edgeColor;
				m.edgeSize = p.edgeSize * mul.edgeSize + add.edgeSize;
				m.textureAddValue = add.textureValue;
				m.sphereAddValue = add.sphereValue;
				m.toonTextureAddValue = add.toonTextureValue;
				m.textureMulValue = mul.textureValue;
				m.sphereMulValue = mul.sphereValue;
				m.toonTextureMulValue = mul.toonTextureValue;
				i++;
			}
		} else {
			for (int i = 0; auto && m:materials) {
				m = Material(pmx->materials[i]);
				i++;
			}
		}


		//ローカル変換行列作成と、IK&付与のための準備
		//#pragma omp parallel for
		for (int i = 0; i < TFOrder.size(); i++) {
			PoseBone* b = bones[TFOrder[i]];
			b->localRotation = b->keyRotation;
			b->localTranslation = b->keyTranslation;
			b->toParent = oRtMatrix(b->origin, b->localRotation, b->localTranslation);

			b->IKLinked = false;
			b->IKRotation = vecQ::Identity;
			b->appendedRotation = vecQ::Identity;
			b->appendedTranslation = vec3(0, 0, 0);
			b->transformResolved = false;
		}

		//ここで表示・外親・IKキーを得ておく
		auto extra = pose.extraKey;

		//TODO:物理前後で分ける。物理前にbonesBeforePhys個、物理後に物理の結果をローカル変形に反映させてから残りのボーンを動かすようにすべし
		for (int i = 0; i < TFOrder.size(); i++) {
			int ib = TFOrder[i];
			auto& pmxb = pmx->bones[ib];
			auto& target = bones[ib];

			//LOG(L"processing #%03d %s(bone#%03d,level#%d)", i, pmxb.name, ib, pmxb.level);

			//付与の解決…ローカル付与はMMDが対応してないのでこっちでも未対応とする
			if (pmxb.appendParent != -1) {
				auto& aparent = bones[pmxb.appendParent];		//付与親情報
				auto& aparentB = pmx->bones[pmxb.appendParent];

				//回転付与
				if (pmxb.IsAppendRotation()) {
					vecQ q = aparentB.IsAppendRotation() ? aparent->appendedRotation : aparent->keyRotation;

					//付与親がIKリンクボーンである場合はここで解決?
					/*
					if (aparent->IKMaster >= 0) {
						int iik = Bone2IK[aparent->IKMaster];
						IKEnabled[iik] = extra.IK[iik].enabled;
						if (IKEnabled[iik]) {
							IK(aparent->IKMaster);
						}
					}
					*/

					//IK回転量
					if (aparent->IKLinked) {
						q = q * vecQ(aparent->IKRotation);
					}

					//付与率
					q = vecQ::Slerp(vecQ::Identity, q, pmxb.appendRatio);

					//注意：rotationはXMVECTORなので普通に*=するとクォータニオン扱いされず変になる
					target->appendedRotation = q * vecQ(target->keyRotation);
					target->localRotation = vecQ(target->appendedRotation) * vecQ(target->IKRotation);
					//LOG(L"append #%d%s<-#%d%s", ib, pmxb.name, pmxb.appendParent, aparentB.name);
				}

				//移動付与
				if (pmxb.IsAppendTranslation()) {
					vec3 tr = aparentB.IsAppendTranslation() ? aparent->appendedTranslation : aparent->keyTranslation;
					tr *= pmxb.appendRatio;
					target->localTranslation = tr + target->keyTranslation;
					target->appendedTranslation = target->localTranslation;	//これでいい？
				}

				//ローカル変換更新
				target->UpdateLocalTransform();
			}


			//IKボーンの場合はIKを解決する→IKリンクボーンのローカル変換が変更される
			if (pmxb.IsIK()) {
				//キーフレームでIKがonになっているかを記録
				int iik = Bone2IK[ib];

				IKEnabled[iik] = extra.IK[iik].enabled;
				//キーフレームでonになってたらIKを解決
				if (IKEnabled[iik]) {
					//LOG(L"solving IK#%d(%s)", ib, pmx->bones[ib].name);
					IK(ib);
				}
			}

			//付与でば:捩じりボーン表示
			/*
			if (pmxb.name.starts_with(L"右腕捩")) {
				std::wstring msg = std::format(L"{}@{} : apRot:{}, localRot:{}, keyRot:{}\n",
					pmxb.name, pmx->name, quat2wstr(target->appendedRotation), quat2wstr(target->localRotation), quat2wstr(target->keyRotation));
				OutputDebugStringW(msg.c_str());
			}
			*/
		}

		//表示フラグ
		visible = extra.show;

		//最終結果を反映
		root->Propagate(root);
		FinishMatrices();
	}

	//timeにおけるポーズ(全ボーンの姿勢)を取得する
	void PoseSolver::Solve(float time, bool integerTime, bool contPhys)
	{
		//ベンチマーク
		/*
		LARGE_INTEGER freqT, beginT;
		QueryPerformanceFrequency(&freqT);
		QueryPerformanceCounter(&beginT);
		*/

		HiPose(solvedPose, time, integerTime, contPhys);
		PoseMining(solvedPose);

		/*
		LARGE_INTEGER endT;
		QueryPerformanceCounter(&endT);
		double elapsed_time = (double)(endT.QuadPart - beginT.QuadPart) * 1E+6 / freqT.QuadPart;
		::LOG(L"*** %.2lf μs for solve pose overall***", elapsed_time);
		*/
	}

	void PoseSolver::FinishMatrices()
	{
		int i = 0;
		for (const auto& b : bones) {
			boneMatrices[i] = XMMatrixTranspose(b->transform);
			i++;
		}
	}

	//IKボーンの有効/無効(iBone番のボーンがそもそもIKボーンではない場合もfalseが返る)
	bool PoseSolver::IKBoneEnabled(int iBone)
	{
		//IKキーがvmdに無い場合は、「IKボーンならIK有効」という事にする
		if (IKEnabled.empty())
			return pmx->bones[iBone].IsIK();

		auto iik = Bone2IK[iBone];
		
		if (iik < 0)
			return false;
		
		return IKEnabled[iik];
	}

	//rootへの回転変換をセットし、親への変換を逆算する(IKの前後ではtranslationは不変)
	//実にIK的な関数
	void IKSetLocalFromGlobalRotation(PoseBone* bone, vecQ q)
	{
		vecQ oldQ = bone->localRotation;	//IK前のローカル回転変換
		vecQ newQ = q * Conjugate(Quaternion::CreateFromRotationMatrix(bone->parent->transform));	//IK後のローカル回転

		bone->localRotation = newQ;
		bone->IKRotation = Conjugate(oldQ) * newQ;
		bone->UpdateLocalTransform();

		//これをやらないと、この関数自体の計算にbone->parent->transformを使うので、今計算しているボーンのワールド座標が他のIKリンクから参照される場合おかしくなる
		bone->transform = bone->toParent * bone->parent->transform;
		bone->transformResolved = true;
	}

	//IK用。ps[idx]を中心として自分より末端のIKリンク全部とターゲットをqで回転させる
	void IKApplyTransform(int idx, vector<vec3>& ps, vector<vecQ>& Qs, vecQ q, vec3& target)
	{
		for (int i = 0; i <= idx; i++) {
			ps[i] = vec3::Transform(ps[i] - ps[idx], q) + ps[idx];
			Qs[i] *= q;
		}
		target = vec3::Transform(target - ps[idx], q) + ps[idx];
	}

	//可動範囲制限の有る関節のヒンジ軸を得る
	vec3 IKHingeAxis(const PMXIKLink& link)
	{
		vec3 hinge;
		vec3 range = link.hi - link.low;
		if (range.x >= range.y && range.x >= range.z) {
			hinge = (abs(link.hi.x) > abs(link.low.x) ? vec3(Sign(link.hi.x), 0, 0) : vec3(Sign(link.low.x), 0, 0));
		} else if (range.y >= range.x && range.y >= range.z) {
			hinge = (abs(link.hi.y) > abs(link.low.y) ? vec3(0, Sign(link.hi.y), 0) : vec3(0, Sign(link.low.y), 0));
		} else {
			hinge = (abs(link.hi.z) > abs(link.low.z) ? vec3(0, 0, Sign(link.hi.z)) : vec3(0, 0, Sign(link.low.z)));
		}
		return hinge;
	}

	//角度制限するための回転を返す
	vecQ IKLimitEulerQ(int j, vector<PMXIKLink>& links, vector<PoseBone*>& linkBs, vector<vecQ>& Qs)
	{
		vecQ parentQ = (j < links.size() - 1) ? Qs[j + 1] : vecQ::CreateFromRotationMatrix(linkBs.back()->parent->transform);
		vecQ relQ = Qs[j] * Conjugate(parentQ);
		//相対回転をオイラー角で解釈して範囲制限
		vec3 euler = relQ.ToEuler();
		euler.Clamp(links[j].low, links[j].hi);
		vecQ limitedQ = vecQ::CreateFromYawPitchRoll(euler) * parentQ;//範囲制限後の姿勢
		vecQ henkaQ = Conjugate(Qs[j]) * limitedQ; //この時点での回転をキャンセルして制限後の姿勢に持っていくための回転
		return henkaQ;
	}

	//oldQからnewQに回転させる単位角までで制限されたクォータニオン
	//制限された単位角までで最後まで回転できる場合はcomplete = trueが入る
	vecQ IKUnitQ(const vecQ& oldQ, const vecQ& newQ, float unit, bool& complete)
	{
		vecQ d = newQ * Conjugate(oldQ);
		float theta = 2.0f * acosf(Clamp(d.w, -1.0f, 1.0f));

		if (theta > unit) {
			complete = false;
			return vecQ::Slerp(vecQ::Identity, d, unit / theta);
		} else {
			complete = true;
			return d;
		}
	}

	//oldQからnewQに回転させるクォータニオンdを単位角までで制限する
	//制限された単位角までで最後まで回転できる場合はcomplete = trueが入る
	vecQ IKUnitQ(const vecQ& d, float unit, bool& complete)
	{
		float theta = 2.0f * acosf(Clamp(d.w, -1.0f, 1.0f));

		if (theta > unit) {
			complete = false;
			return vecQ::Slerp(vecQ::Identity, d, unit / theta);
		} else {
			complete = true;
			return d;
		}
	}

	//IK用。ps[idx]を中心として自分より末端のIKリンク全部とターゲットをqで回転させる。単位角までの制限版
	bool IKApplyTransformWithinUnit(int idx, vector<vec3>& ps, vector<vecQ>& Qs, vecQ q, vec3& target, float unit)
	{
		bool complete = true;
		vecQ u = IKUnitQ(q, unit, complete);
		for (int i = 0; i <= idx; i++) {
			ps[i] = vec3::Transform(ps[i] - ps[idx], u) + ps[idx];
			Qs[i] *= u;
		}
		target = vec3::Transform(target - ps[idx], u) + ps[idx];
		return complete;
	}


#if 0
	//β4までのIK
	//IKの解決
	//注意：IKリンクの子ボーンが枝分かれする場合を考えていない
	// //IKリンクの下にIKリンク & 非IKリンクボーンで枝分かれは不可。IKリンクの下に複数のIKリンクで枝分かれも不可
	//一旦非IKリンクボーン1本挟んでから、そのボーンから枝分かれはOK
	void PoseSolver::IK(int iBone)
	{
		/*
		LARGE_INTEGER freq, time1, time2;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&time1); */

		int i = iBone;
		auto& pmxb = pmx->bones[i];
		auto& targetB = bones[pmxb.IKTarget];

		//IKリンクカウント0とかいう場合は帰る(なんかそういうモデルは実際ある)
		if (pmxb.IKLinkCount == 0)
			return;

		//関連ボーンのワールド変換の確定
		bones[i]->ResolveTransform();
		targetB->ResolveTransform();

		//特に断りが無い限り座標系はroot座標系
		vec3 goal = vec3::Transform(bones[i]->origin, bones[i]->transform);	//IKボーンの位置
		vec3 target = vec3::Transform(targetB->origin, targetB->transform);	//ターゲットボーン(末端の点)の位置

		//最初から十分近づいてるなら何もしない
		if ((target - goal).LengthSquared() < 1e-4) {
			return;
		}

		//IKの目標：リンクボーンを回転させてターゲットボーンの位置(target)をIKボーン(goal)へ近づける
		// 正直、用語的にどうなんだろう、IKボーンの位置がtargetでターゲットボーンはend effectorとかじゃないかいう気はするが仕方ない

		//末端のリンクボーンから順にlinksに
		vector<PMXIKLink>links(pmxb.IKLinkCount);
		vector<PoseBone*>linkBs(pmxb.IKLinkCount);
		vector<vec3>ps(pmxb.IKLinkCount);	//IKの各点のグローバル位置。ps[0]が末端のリンク
		vector<vecQ>Qs(pmxb.IKLinkCount);
		for (int j = 0; j < pmxb.IKLinkCount; j++) {
			links[j] = pmxb.IKLinks[j];
			linkBs[j] = bones[links[j].bone];
			linkBs[j]->IKLinked = true;
			Matrix m = linkBs[j]->ResolveTransform();
			ps[j] = vec3::Transform(linkBs[j]->origin, m);//各ボーンの原点のグローバル位置
			Qs[j] = vecQ::CreateFromRotationMatrix(m);	 //グローバル回転。注意：スケール入りの行列不可
		}


		if (0){//pmxb.IKLinkCount == 1) {
			//LOG(L"Solving 1Link IK:%s(%02d), Link:%s(%02d), Target:%s(%02d)", pmxb.name, i, pmx->m_bs[links[0].bone].name, links[0].bone, pmx->m_bs[pmxb.IKTarget].name, pmxb.IKTarget);
			//IKリンクが1つしかない場合、単純にgoalの方向を向けるだけ
			vecQ q = vecQ::FromToRotation(target - ps[0], goal - ps[0]);
			IKApplyTransform(0, ps, Qs, q, target);
			//角度制限
			if (links[0].isLimit) {
				vecQ henkaQ = IKLimitEulerQ(0, links, linkBs, Qs);
				IKApplyTransform(0, ps, Qs, henkaQ, target);
			}
			//root座標系で計算してたのでローカル変換に戻す
			IKSetLocalFromGlobalRotation(linkBs[0], Qs[0]);
		} else if (0){//pmxb.IKLinkCount == 2) {
			//2ボーンIK
			// ps[1]根元、ps[0]がジョイント、targetが先っちょとする
			// 注意　①根元の可動範囲制限が考慮されてない　②関節の可動範囲は1軸に限定される
			float A = Length(ps[1] - ps[0]), B = Length(ps[0] - target), C = Length(ps[1] - goal);
			float rad = acos(Clamp((A * A + B * B - C * C) / (2 * A * B), -1, 1));	//余弦定理でps[0]を何度にすればいいか調べる
			//LOG(L"solving 2bone IK for %s deg:%.2f", pmxb.name, rad/PI*180);

			//関節の回転軸を決める
			vec3 axis = vec3(0, 1, 0);
			float range = PI;	//可動範囲
			float minrange = 0;	//最小可動範囲(完全に伸びた状態が0、屈曲した状態がπ)
			if (links[0].isLimit) {
				//hiとlowで0から見て可動範囲が大きい方に曲げるように軸が選ばれる
				axis = IKHingeAxis(links[0]);
				//可動範囲が大きい方に最大何度曲げられるか？
				float hidot = Dot(links[0].hi, axis);
				float lodot = Dot(links[0].low, axis);
				range = max(abs(hidot), abs(lodot));
				minrange = (Sign(hidot) != Sign(lodot)) ? 0 : min(abs(hidot), abs(lodot));
			} else {
				//可動範囲の制限が無い場合はCrossで求める…書いたけどまだ検証してない
				vec3 scale, trans;
				vecQ toLocal;
				Matrix(linkBs[1]->parent->transform).Decompose(scale, toLocal, trans);	//注意：スケールをまだ考慮してない
				vec3 b10 = Rotate(ps[0] - ps[1], toLocal);	//根元の関節の座標系での向きを知りたいので
				vec3 b0g = Rotate(goal - ps[0], toLocal);
				axis = Normalize(Cross(b10, b0g));
				//一直線になってたらとりあえずZ軸とする
				if (Length(axis) == 0) {
					axis = vec3(0, 0, 1);
				}
			}

			// 関節を初期状態にした時の角度を求める
			A = Length(linkBs[0]->origin - linkBs[1]->origin);
			B = Length(linkBs[0]->origin - targetB->origin);
			C = Length(linkBs[1]->origin - targetB->origin);
			float straight = acos(Clamp((A * A + B * B - C * C) / (2 * A * B), -1, 1));
			minrange = max(minrange, PI - straight);	//最小可動範囲の設定：初期状態より伸びないようにする

			//一旦、関節を完全に伸ばす
			vecQ mageQ = vecQ::FromToRotation(target - ps[0], ps[0] - ps[1]);

			//ヒンジ軸周りに回す(axisはそのままだとlinkBs[1]の座標系なので、ワールドに変換する)
			mageQ *= vecQ::CreateFromAxisAngle(vec3::TransformNormal(axis, linkBs[1]->transform), max(minrange, min(PI - rad, range)));
			mageQ = Normalize(mageQ);
			IKApplyTransform(0, ps, Qs, mageQ, target);

			//最後に、根元の方向をゴール方向へ向ける
			vecQ q2 = vecQ::FromToRotation(target - ps[1], goal - ps[1]);
			IKApplyTransform(1, ps, Qs, q2, target);

			//debug
			//A = Length(ps[1] - ps[0]); B = Length(ps[0] - target); C = Length(ps[1] - target);
			//LOG(L"result deg:%.2f", acos(Clamp((A* A + B * B - C * C) / (2 * A * B), -1, 1)) * 180 / PI);

			//root座標系で計算してたのでローカル変換に戻す(根元→関節の順)
			IKSetLocalFromGlobalRotation(linkBs[1], Qs[1]);
			IKSetLocalFromGlobalRotation(linkBs[0], Qs[0]);

			//debug
			//target = vec3::Transform(targetB->origin, targetB->transform);
			//LOG(L"solved T%s  /  G%s deg:%.2f, err:%.2f", vec2wstr(target).c_str(), vec2wstr(goal).c_str(), rad/PI*180.0f, Length(target-goal));
		} else {
			//CCD IK
			// 
			//LOG(L"starting resolve IK %s", pmxb.name);

			//グローバル回転をリストにいれとく

			//ヒストリーバッファ
			int numHistory = pmxb.IKLinkCount * pmxb.IKLoopCount;
			vec3* pH = new vec3[numHistory];
			vecQ* QH = new vecQ[numHistory];
			float* errH = new float[pmxb.IKLoopCount];

			//ヒンジ軸。とりあえず可動範囲制限は1軸可動のみサポート
			vector<vec3>hinges;	hinges.resize(pmxb.IKLinkCount);
			for (int j = 0; j < pmxb.IKLinkCount; j++)
				hinges[j] = IKHingeAxis(links[j]);

			//根元からゴールまでの直線距離
			float goalLength = Length(ps[0] - goal);
			//根元からターゲットまでの各関節の長さの合計
			float totalJointsLength = 0;
			for (int j = 0; j < links.size(); j++) {
				if (j == 0)
					totalJointsLength += Length(ps[j] - target);
				else
					totalJointsLength += Length(ps[j] - ps[j - 1]);
			}

			//全部のボーンとゴールが一直線になってる事に対策する。ルート→ターゲットの距離がルート→ゴールの距離未満になるよう関節を曲げとく
			for (int iter = 0; iter < 20; iter++) {
				if (Length(target - ps[0]) < goalLength) {
					break;
				}
				for (int j = 1; j < pmxb.IKLinkCount; j++) {
					if (links[j].isLimit) {
						vecQ parentQ = (j < pmxb.IKLinkCount - 1) ? Qs[j + 1] : vecQ::CreateFromRotationMatrix(linkBs.back()->parent->transform);
						vecQ hogQ = Conjugate(parentQ) * Normalize(vecQ(hinges[j], PI / 5.5)) * parentQ;	//normalizeがあるので
						IKApplyTransform(j, ps, Qs, hogQ, target);
					}
				}
			}

			//target-goalの距離の2乗がこの値以下になったら終了
			float threshold = 0.01;

			//ps, QsについてCCD-IKを解く
			vec3 last_target = goal;
			int loops = 0;
			float leastErr = 1E+10;	//今まででエラーが最小になってた時のエラー値
			for (int iter = 0; iter < pmxb.IKLoopCount; iter++) {
				//収束判定
				float err = (target - goal).LengthSquared();
				if (err < threshold)	//収束した
					break;
				if (err > leastErr * 2)	//発散に向かっているので打ち切る
					break;
				leastErr = min(leastErr, err);

				//ロックが発生しているか関節の長さより遠い所にゴールがある
				if ((last_target - target).LengthSquared() < threshold) {
					//関節の長さより遠い所にゴールがあるなら、伸ばし切ってると思われるため終わる
					if (totalJointsLength <= goalLength)
						break;
					/*
					//ロックが発生してる。ほぐす
					for (int j = 0; j < links.size(); j++) {
						//ヒンジ軸回りに少し曲げる
						if (links[j].isLimit) {
							vecQ parentQ = (j < links.size() - 1) ? Qs[j + 1] : vecQ::CreateFromRotationMatrix(linkBs.back()->parent->transform);
							vecQ hogQ = Conjugate(parentQ) * Normalize(vecQ(hinges[j], 0.1)) * parentQ;
							IKApplyTransform(j, ps, Qs, hogQ, target);
						}
					}
					*/
				}

				last_target = target;
				//末端ボーンから順にtarget向き→goal向きへと回転させる
				for (int j = 0; j < pmxb.IKLinkCount; j++) {
					//軸制限の有無で共通の部分。まず、自由に動ける場合の回転qを決定
					vecQ q = vecQ::FromToRotation(target - ps[j], goal - ps[j]);
					float rad = acos(q.w) * 2;	//qは何ラジアンの回転か？
					if (abs(rad) > 1e-5)
						q = vecQ::Slerp(vecQ::Identity, q, min(1, pmxb.IKAngle / rad));	//単位角までの角度に制限する

					if (links[j].isLimit) {
						//回転の適用
						IKApplyTransform(j, ps, Qs, q, target);

						//↑の回転適用後の姿勢について、一つ根元寄りのリンクに対する相対的な回転を得て、適用
						vecQ henkaQ = IKLimitEulerQ(j, links, linkBs, Qs);
						IKApplyTransform(j, ps, Qs, henkaQ, target);
					} else {
						//軸制限なしならそのまま回転の適用
						IKApplyTransform(j, ps, Qs, q, target);
					}
				}

				for (int j = 0; j < pmxb.IKLinkCount; j++) {
					int hIdx = loops * pmxb.IKLinkCount + j;
					pH[hIdx] = ps[j];
					QH[hIdx] = Qs[j];
				}
				errH[loops] = (target - goal).LengthSquared();

				//LOG(L"loops#%.2d %s  : %f to go", loops, vec2wstr(target).c_str(), Length(target - goal));
				loops++;
			}

			//一番誤差が低かった時のps・Qsを採用
			int bestIdx = 0;
			float best = 1E+10;
			for (int j = 0; j < loops; j++) {
				if (best > errH[j] && isfinite(errH[j])) {
					best = errH[j];
					bestIdx = j;
				}
			}
			//LOG(L"bestIdx = %d", bestIdx);
			for (int j = 0; j < pmxb.IKLinkCount; j++) {
				int hIdx = bestIdx * pmxb.IKLinkCount + j;
				ps[j] = pH[hIdx];
				Qs[j] = QH[hIdx];
			}


			//解けたら根元から順にps,Qsから、rootへの変換を計算し、さらに親への変換も得る
			vec3 o;
			for (int j = pmxb.IKLinkCount - 1; j >= 0; j--) {
				//LOG(L"ps[%d] = %s", j, vec2wstr(ps[j]).c_str());
				IKSetLocalFromGlobalRotation(linkBs[j], Qs[j]);
			}


			//debug
			//target = vec3::Transform(targetB->origin, targetB->transform);
			//LOG(L"solved #%d, T%s  /  G%s ", loops, vec2wstr(target).c_str(), vec2wstr(goal).c_str());

			delete[] pH;
			delete[] QH;
			delete[] errH;
		}// CCD-IK

		//QueryPerformanceCounter(&time2);
		//OutputDebugString(std::format(L"IK solved {}us \n", (double)(time2.QuadPart-time1.QuadPart)/freq.QuadPart * 1e+6).c_str());

	}// IK()
#endif




	//IKボーン(ボーン番号ib)についてIKを解いてIKリンクボーンのローカル変換を更新する
	//前提：
	// 1.リンクボーン群は全てend effectorの先祖ボーンである。
	// 2.リンクボーン[0]はend efectorの親、リンクボーン[i+1]はリンクボーン[i]の親…という親子構造になっている
	void PoseSolver::IK(int ib)
	{
		/*
		constexpr bool dbg = true;
		constexpr auto deb = [&](const std::wstring& msg) {
			if (dbg)
				OutputDebugStringW(msg.c_str());
		};
		
		LARGE_INTEGER time1,time2,freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&time1);
		*/

		//target(IKボーン) と end effector(IKターゲットボーン)
		//用語に混乱があるけどMMD用語とIK用語で違うので、IK用語に合わせて表記する
		auto& tgtP = pmx->bones[ib];
		auto* tgtB = bones[ib];
		tgtB->ResolveTransform();
		vec3 tgtW = vec3::Transform(tgtB->origin, tgtB->transform);	//目標地点のワールド座標。IKの処理の間は一定

		auto* eeB = bones[tgtP.IKTarget];
		eeB->ResolveTransform();	//リンクボーン群は全てend effectorの親(先祖)なので、リンクボーン群のワールド変換も確定する	

		//auto& eeP = pmx->bones[tgtP.IKTarget];
		//deb(std::format(L"----- solving IK start -----\n"));
		//deb(std::format(L"target : ({}){}, end effector : ({}){}\n", ib, tgtP.name, tgtP.IKTarget, eeP.name));

		//IKリンク情報から得られるIK計算用の関節情報
		auto& joints = m_IKJoints[Bone2IK[ib]];
		if (joints.empty())
			return;

		//IK前の姿勢を保存
		for (auto&& j : joints) {
			j.originalRotation = j.bone->localRotation;
		}
		//伸ばした状態でないと届かない位置にターゲットがあるので延ばす…というのはやると不連続な所ができるのでやらない方が良い

		int loopcount = max(1,min(tgtP.IKLoopCount, 255));	//PMX仕様によると本家では255が最大とのこと
		for (int loop = 0; loop < loopcount; loop++) {
			for (int k = 0; k < joints.size(); k++) {
				//完全に固定されているジョイントならとばす
				if (joints[k].fixed)
					continue;
				
				auto* L = joints[k].bone;
				
				//1.ワールド座標系でend effectorをtargetに向かって回転させるクォータニオンを作る
				vec3 jointW = vec3::Transform(L->origin, L->transform);  // 関節位置
				vec3 eeW = vec3::Transform(eeB->origin, eeB->transform); // エンドエフェクタ位置
				vec3 dirE = Normalize(eeW - jointW);
				vec3 dirT = Normalize(tgtW - jointW);

				//向きが合ってればループを抜ける
				float err = (dirE - dirT).LengthSquared();
				//deb(std::format(L"#{} {}, err:{}\n", loop, pmx->bones[links[k].bone].name, err));
				if (err < 1e-7) {
					//deb(L"converged!\n");
					loop = loopcount;	//外のループも抜ける
					break;
				}

				//end effector方向→ターゲット方向へ回転(ワールド)
				vecQ deltaWQ = vecQ::FromToRotation(dirE, dirT);

				//2.ワールド回転をローカルへ変換
				vecQ wq = vecQ::CreateFromRotationMatrix(L->parent->transform);	//親→ワールド
				vecQ maxQ = Normalize(vecQ(L->localRotation) * wq * deltaWQ * Conjugate(wq));	//制限なしで回転できる場合のローカル回転量
				
				//3.回転量を単位角までに制限。根元ほど回転を大きくさせる(k+1)が大事らしい
				vecQ newQ = L->localRotation;
				newQ.RotateTowards(maxQ, tgtP.IKAngle * (k+1));	
				
				//4.軸制限
				if (joints[k].limit)
					newQ = joints[k].Limit(newQ);

				//5.Lと子孫のローカル・グローバル変換を更新
				L->localRotation = newQ;
				/*
				L->UpdateLocalTransform();
				L->ResolveTransform();
				L->Propagate(L);	//end effectorはLの子孫なので、これでeeBも更新される
				*/
				//IKと関係ある範囲で最低限更新する版
				L->toParent = oRtMatrix(L->origin, L->localRotation, L->localTranslation);
				for (int j = k; j >= 0; j--) {
					auto* b = joints[j].bone;
					b->transform = b->toParent * b->parent->transform;
				}
				eeB->transform = eeB->toParent * eeB->parent->transform;
			}
		}
		//IK回転量を格納
		for (auto&& l : joints) {
			auto* b = l.bone;
			b->IKLinked = true;
			//「元の回転量」はkeyRotationなのかIK処理前のlocalRotationなのかちょっと悩むところ
			//b->IKRotation = Conjugate(b->keyRotation) * vecQ(b->localRotation);	
			b->IKRotation = Conjugate(l.originalRotation) * vecQ(b->localRotation);
		}
		//IKリンクボーン群とその子孫のワールド変換行列更新
		auto* rootB = joints.back().bone;
		rootB->PropagateUnresolved(rootB);	
		rootB->ResolveTransform();
		rootB->Propagate(rootB);

		/*
		QueryPerformanceCounter(&time2);
		deb(std::format(L"IK {} solved ... links:{} time:{}us \n", tgtP.name, links.size(), (double)(time2.QuadPart-time1.QuadPart)/freq.QuadPart * 1e+6));
		*/
	}



	/******************************************************************************************/
	// 物理 : benikabocha様作 sabaを 大いに参考にさせていただきました
	/******************************************************************************************/
	struct MMDFilterCallback : public btOverlapFilterCallback
	{
		bool needBroadphaseCollision(btBroadphaseProxy* proxy0, btBroadphaseProxy* proxy1) const override
		{
			auto findIt = std::find_if(
				m_nonFilterProxy.begin(),
				m_nonFilterProxy.end(),
				[proxy0, proxy1](const auto& x) {return x == proxy0 || x == proxy1; }
			);
			if (findIt != m_nonFilterProxy.end()) {
				return true;
			}
			bool collides = (proxy0->m_collisionFilterGroup & proxy1->m_collisionFilterMask) != 0;
			collides = collides && (proxy1->m_collisionFilterGroup & proxy0->m_collisionFilterMask);
			return collides;
		}

		std::vector<btBroadphaseProxy*> m_nonFilterProxy;	//これに加えられている物体はグループマスクによる判定をしない(どのグループとも衝突する)
	};

	//ボーンと関連づいてない剛体用
	class DefaultMotionState : public MMDMotionState {
	private:
		btTransform	m_initialTransform;
		btTransform	m_transform;
	public:
		DefaultMotionState(const XMMATRIX& transform)
		{
			Matrix tr = transform;
			m_transform.setFromOpenGLMatrix(&tr._11);
			m_initialTransform = m_transform;
		}

		void getWorldTransform(btTransform& worldTransform) const override
		{
			worldTransform = m_transform;
		}

		void setWorldTransform(const btTransform& worldTransform) override
		{
			m_transform = worldTransform;
		}

		virtual void Reset() override
		{
			m_transform = m_initialTransform;
		}

		virtual void ReflectGlobalTransform() override
		{
		}
	};

	//物理演算剛体用
	class DynamicMotionState : public MMDMotionState {
	private:
		PoseBone* m_bone;
		Matrix	m_offset;		//剛体座標系→bone座標系変換
		Matrix	m_invOffset;	//bone座標系→剛体座標系変換
		btTransform	m_transform;
		bool		m_override;

		btTransform	m_initialTransform;
	public:
		DynamicMotionState(PoseBone* bone, const XMMATRIX& offset, bool override = true)
			: m_bone(bone), m_offset(offset), m_override(override)
		{
			//追加
			Matrix tr = offset;
			m_transform.setFromOpenGLMatrix(&tr._11);
			m_initialTransform = m_transform;

			m_invOffset = Matrix(offset).Invert();
			Reset();
		}

		void getWorldTransform(btTransform& worldTransform) const override
		{
			worldTransform = m_transform;
		}

		void setWorldTransform(const btTransform& worldTransform) override
		{
			m_transform = worldTransform;
		}

		void Reset() override
		{
			Matrix global = m_offset * Matrix(m_bone->transform);
			m_transform.setFromOpenGLMatrix(&global._11);

			//↓から↑に変更(作成時の姿勢を代入すると、キーフレームアニメによる変形が無視されてしまうので)
			//m_transform = m_initialTransform;
		}

		void ReflectGlobalTransform() override
		{
			if (m_override) {
				Matrix world;	//剛体→ワールド変換
				m_transform.getOpenGLMatrix(&world._11);
				Matrix btGlobal = m_invOffset * world;
				m_bone->transform = btGlobal;
				//TODO:物理後変形の場合の事を考える
				m_bone->PropagateUnresolved(m_bone);	//自分のボーンのワールド変換は確定済みだが子孫ボーンのワールド変換は未定、という状態にする(自分のローカル変換はおかしな状態になっている)
				m_bone->transformResolved = true;
			}
		}

	};

	//物理+ボーン位置合わせ剛体用...移動分はボーンの位置、回転分だけ物理というボーン
	class DynamicAndBoneMergeMotionState : public MMDMotionState
	{
	private:
		PoseBone* m_bone;
		Matrix	m_offset;
		Matrix m_invOffset;
		btTransform	m_transform;
		bool		m_override;

		btTransform m_initialTransform;
	public:
		DynamicAndBoneMergeMotionState(PoseBone* bone, const XMMATRIX& offset, bool override = true)
			: m_bone(bone)
			, m_offset(offset)
			, m_override(override)
		{
			//追加
			Matrix tr = offset;
			m_transform.setFromOpenGLMatrix(&tr._11);
			m_initialTransform = m_transform;

			m_invOffset = Matrix(offset).Invert();
			Reset();
		}

		void getWorldTransform(btTransform& worldTransform) const override
		{
			worldTransform = m_transform;
		}

		void setWorldTransform(const btTransform& worldTransform) override
		{
			m_transform = worldTransform;
		}

		void Reset() override
		{
			Matrix global = m_offset * Matrix(m_bone->transform);
			m_transform.setFromOpenGLMatrix(&global._11);

			//↑から↓に変更
			//m_transform = m_initialTransform;
		}

		void ReflectGlobalTransform() override
		{
			//メモ m_offset : rigid->bone 行列

			if (m_override) {
				m_bone->ResolveTransform();
				Matrix world;
				m_transform.getOpenGLMatrix(&world._11);
				Matrix global = m_offset * m_bone->transform;
				world._41 = global._41;	world._42 = global._42;	world._43 = global._43;
				m_bone->transform = m_invOffset * world;
				m_bone->PropagateUnresolved(m_bone);
				m_bone->transformResolved = true;
			}
		}


	};

	//ボーン追従剛体用
	class KinematicMotionState : public MMDMotionState {
	private:
		PoseBone* m_bone;
		XMMATRIX m_offset;
	public:
		KinematicMotionState(PoseBone* bone, const XMMATRIX& offset) : m_bone(bone), m_offset(offset) {}

		void getWorldTransform(btTransform& worldTransform) const override
		{
			Matrix m;
			if (m_bone != nullptr)
				m = Matrix(m_offset) * Matrix(m_bone->transform);
			else
				m = m_offset;

			worldTransform.setFromOpenGLMatrix(&m._11);
		}

		void setWorldTransform(const btTransform& worldTransform) override {}
		void Reset() override {}
		void ReflectGlobalTransform() override {}
	};



	Physics::Physics()
	{
		m_broadphase = std::make_unique<btDbvtBroadphase>();
		m_collisionConfig = std::make_unique<btDefaultCollisionConfiguration>();
		m_dispatcher = std::make_unique<btCollisionDispatcher>(m_collisionConfig.get());

		m_solver = std::make_unique<btSequentialImpulseConstraintSolver>();

		m_world = std::make_unique<btDiscreteDynamicsWorld>(
			m_dispatcher.get(),
			m_broadphase.get(),
			m_solver.get(),
			m_collisionConfig.get()
		);

		m_world->setGravity(btVector3(0, -9.8f * 10.0f, 0));

		m_groundShape = std::make_unique<btStaticPlaneShape>(btVector3(0, 1, 0), 0.0f);

		btTransform groundTransform;
		groundTransform.setIdentity();

		m_groundMS = std::make_unique<btDefaultMotionState>(groundTransform);

		btRigidBody::btRigidBodyConstructionInfo groundInfo(0, m_groundMS.get(), m_groundShape.get(), btVector3(0, 0, 0));
		m_groundRB = std::make_unique<btRigidBody>(groundInfo);

		m_world->addRigidBody(m_groundRB.get());

		auto filterCB = std::make_unique<MMDFilterCallback>();
		filterCB->m_nonFilterProxy.push_back(m_groundRB->getBroadphaseProxy());
		m_world->getPairCache()->setOverlapFilterCallback(filterCB.get());
		m_filterCB = std::move(filterCB);
	}

	Physics::~Physics()
	{
		if (m_world != nullptr && m_groundRB != nullptr)
			m_world->removeRigidBody(m_groundRB.get());

		m_broadphase = nullptr;
		m_collisionConfig = nullptr;
		m_dispatcher = nullptr;
		m_solver = nullptr;
		m_world = nullptr;
		m_groundShape = nullptr;
		m_groundMS = nullptr;
		m_groundRB = nullptr;
	}

	void Physics::Update(double dt, bool active)
	{
		m_dt = dt;

		for (auto& n : m_notifees)
			n->OnUpdatePhysicsBegin(active);

		if (m_world != nullptr)
			m_world->stepSimulation(dt, maxSubsteps, static_cast<btScalar>(1.0 / fps));

		for (auto& n : m_notifees)
			n->OnUpdatePhysicsEnd();
	}

	void Physics::AddRigidBody(RigidBody* rb)
	{
		m_world->addRigidBody(rb->rigidBody.get(), 1 << rb->group, rb->groupMask);
	}

	void Physics::RemoveRigidBody(RigidBody* rb)
	{
		m_world->removeRigidBody(rb->rigidBody.get());
	}

	void Physics::AddJoint(Joint* joint)
	{
		if (joint->constraint != nullptr)
			m_world->addConstraint(joint->constraint.get());
	}

	void Physics::RemoveJoint(Joint* joint)
	{
		if (joint->constraint != nullptr)
			m_world->removeConstraint(joint->constraint.get());
	}

	void Physics::AddNotifee(PoseSolver* solver)
	{
		m_notifees.push_back(solver);
	}

	void Physics::DeleteNotifee(PoseSolver* solver)
	{
		auto idx = find(m_notifees.begin(), m_notifees.end(), solver);
		m_notifees.erase(idx);
	}

	void Physics::Reset()
	{
		for (auto& n : m_notifees)
			n->OnResetPhysicsBegin();

		//↓は別にいらんのではないかという気がするのでやらないことにした
		//Updateを呼ぶとイベントハンドラも呼ばれるので直接stepSimulationする
		//if (m_world != nullptr)
		//	m_world->stepSimulation(1/60.0, maxSubsteps, static_cast<btScalar>(1.0 / fps));

		for (auto& n : m_notifees)
			n->OnResetPhysicsEnd();
	}

	void Physics::Prewarm(int frames, float timePerFrame)
	{

		for (int i = 0; i < frames; i++) {
			for (auto& n : m_notifees)
				n->OnPrewarmPhysics(i, frames);

			Update(timePerFrame);

			//OnPrewarmPhysicsはi==0の時に初期スタンスになるので、この時Reset()して剛体の運動量を0にする
			if (i == 0)
				Reset();
		}
		Reset();
	}

	void Physics::GroundEnable(bool enable)
	{
		if (enable)
			m_groundRB->setCollisionFlags(m_groundRB->getCollisionFlags() & ~btCollisionObject::CF_NO_CONTACT_RESPONSE);
		else
			m_groundRB->setCollisionFlags(m_groundRB->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);
	}



	//model内のiRB番の剛体情報をもとに、Bullet用剛体オブジェクトを作る
	RigidBody::RigidBody(const PMXModel* model, int iRB, const PoseSolver* solver)
	{
		m_physics = solver->physics;

		PMXBody r = model->bodies[iRB];
		switch (r.boxKind) {
		case 0:
			m_shape = std::make_unique<btSphereShape>(r.boxSize.x);
			break;
		case 1:
			m_shape = std::make_unique<btBoxShape>(btVector3(r.boxSize.x, r.boxSize.y, r.boxSize.z));
			break;
		case 2:
			m_shape = std::make_unique<btCapsuleShape>(r.boxSize.x, r.boxSize.y);
			break;
		default:
			break;
		}

		m_shape->setMargin(0.01);	//本家ではどうなのか分からない

		btScalar mass(0);
		btVector3 localInertia(0, 0, 0);
		PoseBone* b = r.bone >= 0 ? solver->bones[r.bone] : nullptr;
		bone = b;

		if (r.mode != 0) {
			mass = r.mass;
		}
		if (mass != 0) {
			m_shape->calculateLocalInertia(mass, localInertia);
		}

		Matrix rotMat = Matrix::CreateFromYawPitchRoll(r.rotation);
		Matrix translateMat = Matrix::CreateTranslation(r.position);
		Matrix rbMat = rotMat * translateMat;

		m_offsetMat = (b != nullptr) ? rbMat * Matrix(b->transform).Invert() : rbMat * Matrix(solver->root->transform).Invert();

		btMotionState* motionState = nullptr;


		if (r.mode == 0) {
			m_kinematicMotionState = std::make_unique<KinematicMotionState>(b, m_offsetMat);
			motionState = m_kinematicMotionState.get();
		} else {
			if (b != nullptr) {
				if (r.mode == 1) {
					m_activeMotionState = std::make_unique<DynamicMotionState>(b, m_offsetMat);
					m_kinematicMotionState = std::make_unique<KinematicMotionState>(b, m_offsetMat);
					motionState = m_activeMotionState.get();
				} else if (r.mode == 2) {
					m_activeMotionState = std::make_unique<DynamicAndBoneMergeMotionState>(b, m_offsetMat);
					m_kinematicMotionState = std::make_unique<KinematicMotionState>(b, m_offsetMat);
					motionState = m_activeMotionState.get();
				}
			} else {
				m_activeMotionState = std::make_unique<DefaultMotionState>(m_offsetMat);
				m_kinematicMotionState = std::make_unique<KinematicMotionState>(b, m_offsetMat);
				motionState = m_activeMotionState.get();
			}
		}

		btRigidBody::btRigidBodyConstructionInfo rbInfo(mass, motionState, m_shape.get(), localInertia);
		rbInfo.m_linearDamping = r.positionDamping;
		rbInfo.m_angularDamping = r.rotationDamping;
		rbInfo.m_restitution = r.restitution;
		rbInfo.m_friction = r.friction;
		//rbInfo.m_additionalDamping = true;
		//rbInfo.m_additionalAngularDampingFactor = 0.5;
		//rbInfo.m_additionalDampingFactor = 0.5;

		rigidBody = std::make_unique<btRigidBody>(rbInfo);
		rigidBody->setUserPointer(this);
		rigidBody->setSleepingThresholds(0.01f, 0.1f * PI / 180.0f);
		rigidBody->setActivationState(DISABLE_DEACTIVATION);
		if (r.mode == 0)
			rigidBody->setCollisionFlags(rigidBody->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);

		mode = r.mode;
		group = r.group;
		groupMask = r.passGroup;
	}

	void RigidBody::SetActivation(bool active)
	{
		if (mode != 0) {
			if (active) {
				//2024-1206更新
				//kinematicな状態からdynamicな状態に切り替わる場合は、kinematicの位置・姿勢を引き継ぐ
				//(dynamic→kinematicの場合はボーンの状態そのままなので引継ぎの必要はない)
				//これをやらずにkinematic→dynamicに切り替えただけだと最後にdynamicだった時の位置・姿勢にワープする
				//※dynamic→dynamicでも同じコードが走るが問題はない
				btTransform currentTransform;
				rigidBody->getMotionState()->getWorldTransform(currentTransform);
				rigidBody->setCollisionFlags(rigidBody->getCollisionFlags() & ~btCollisionObject::CF_KINEMATIC_OBJECT);
				rigidBody->setMotionState(m_activeMotionState.get());
				rigidBody->getMotionState()->setWorldTransform(currentTransform);
				rigidBody->setWorldTransform(currentTransform);

				//↓は更新前のコード
				//rigidBody->setCollisionFlags(rigidBody->getCollisionFlags() & ~btCollisionObject::CF_KINEMATIC_OBJECT);
				//rigidBody->setMotionState(m_activeMotionState.get());
			} else {
				rigidBody->setCollisionFlags(rigidBody->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
				rigidBody->setMotionState(m_kinematicMotionState.get());
			}
		} else {
			rigidBody->setMotionState(m_kinematicMotionState.get());
		}
	}

	void RigidBody::ResetTransform()
	{
		if (m_activeMotionState != nullptr)
			m_activeMotionState->Reset();
	}

	void RigidBody::Reset()
	{
		auto cache = m_physics->m_world->getPairCache();
		if (cache != nullptr) {
			auto dispatcher = m_physics->m_world->getDispatcher();
			cache->cleanProxyFromPairs(rigidBody->getBroadphaseHandle(), dispatcher);
		}
		rigidBody->setAngularVelocity(btVector3(0, 0, 0));
		rigidBody->setLinearVelocity(btVector3(0, 0, 0));
		rigidBody->clearForces();
		//追加
		rigidBody->setTurnVelocity(btVector3(0, 0, 0));
		rigidBody->setInterpolationAngularVelocity(btVector3(0, 0, 0));
		rigidBody->setInterpolationLinearVelocity(btVector3(0, 0, 0));

		if (bone) {
			Matrix global = m_offsetMat * bone->transform;
			btTransform tr;
			tr.setFromOpenGLMatrix(&global._11);
			rigidBody->setInterpolationWorldTransform(tr);
		}
	}

	void RigidBody::ReflectGlobalTransform()
	{
		if (m_activeMotionState != nullptr && physKey)
			m_activeMotionState->ReflectGlobalTransform();

		if (m_kinematicMotionState != nullptr)
			m_kinematicMotionState->ReflectGlobalTransform();
	}

	void RigidBody::CalcLocalTransform()
	{
		if (bone != nullptr) {
			auto parent = bone->parent;
			if (parent != nullptr) {
				auto local = Matrix(bone->transform) * Matrix(parent->transform).Invert();
				bone->toParent = local;
			}
		}
	}

	XMMATRIX RigidBody::GetTransform()
	{
		btTransform transform = rigidBody->getCenterOfMassTransform();
		Matrix mat;
		transform.getOpenGLMatrix(&mat._11);
		return mat;
	}

	Joint::Joint(const PMXModel* model, int index, RigidBody* rigidBodyA, RigidBody* rigidBodyB)
	{
		//同じ剛体に繋がっていたり無効な剛体に繋がっているジョイントは作らない
		//(同じ剛体に繋がっているジョイントはbullet内でassertによって弾かれてしまうので)
		if ( (rigidBodyA == rigidBodyB) || (!rigidBodyA) || (!rigidBodyB)) {
			src = -1;
			return;
		}

		src = index;

		PMXJoint pmxJoint = model->joints[index];
		btMatrix3x3 rotMat;
		rotMat.setEulerZYX(pmxJoint.rotation.x, pmxJoint.rotation.y, pmxJoint.rotation.z);

		btTransform transform;
		transform.setIdentity();
		transform.setOrigin(btVector3(pmxJoint.position.x, pmxJoint.position.y, pmxJoint.position.z));
		transform.setBasis(rotMat);

		btTransform invAr = rigidBodyA->rigidBody->getWorldTransform().inverse();
		btTransform invBr = rigidBodyB->rigidBody->getWorldTransform().inverse();
		auto invA = invAr * transform;
		auto invB = invBr * transform;

		switch (pmxJoint.kind) {
		case 0: {
			//バネ付き6DoF
			auto constraint = std::make_unique<btGeneric6DofSpringConstraint>(*rigidBodyA->rigidBody, *rigidBodyB->rigidBody, invA, invB, true);
			//constraint->setUseFrameOffset(false);	//2.75互換の挙動になる？
			constraint->setLinearLowerLimit(btVector3(pmxJoint.moveLo.x, pmxJoint.moveLo.y, pmxJoint.moveLo.z));
			constraint->setLinearUpperLimit(btVector3(pmxJoint.moveHi.x, pmxJoint.moveHi.y, pmxJoint.moveHi.z));

			constraint->setAngularLowerLimit(btVector3(pmxJoint.angleLo.x, pmxJoint.angleLo.y, pmxJoint.angleLo.z));
			constraint->setAngularUpperLimit(btVector3(pmxJoint.angleHi.x, pmxJoint.angleHi.y, pmxJoint.angleHi.z));

			if (pmxJoint.springMove.x != 0) {
				constraint->enableSpring(0, true);
				constraint->setStiffness(0, pmxJoint.springMove.x);
			}
			if (pmxJoint.springMove.y != 0) {
				constraint->enableSpring(1, true);
				constraint->setStiffness(1, pmxJoint.springMove.y);
			}
			if (pmxJoint.springMove.z != 0) {
				constraint->enableSpring(2, true);
				constraint->setStiffness(2, pmxJoint.springMove.z);
			}
			if (pmxJoint.springRotate.x != 0) {
				constraint->enableSpring(3, true);
				constraint->setStiffness(3, pmxJoint.springRotate.x);
			}
			if (pmxJoint.springRotate.y != 0) {
				constraint->enableSpring(4, true);
				constraint->setStiffness(4, pmxJoint.springRotate.y);
			}
			if (pmxJoint.springRotate.z != 0) {
				constraint->enableSpring(5, true);
				constraint->setStiffness(5, pmxJoint.springRotate.z);
			}
			this->constraint = std::move(constraint);
			break;
		}
		case 1: {
			//6DoF
			auto constraint = std::make_unique<btGeneric6DofConstraint>(*rigidBodyA->rigidBody, *rigidBodyB->rigidBody, invA, invB, true);
			constraint->setLinearLowerLimit(btVector3(pmxJoint.moveLo.x, pmxJoint.moveLo.y, pmxJoint.moveLo.z));
			constraint->setLinearUpperLimit(btVector3(pmxJoint.moveHi.x, pmxJoint.moveHi.y, pmxJoint.moveHi.z));
			constraint->setAngularLowerLimit(btVector3(pmxJoint.angleLo.x, pmxJoint.angleLo.y, pmxJoint.angleLo.z));
			constraint->setAngularUpperLimit(btVector3(pmxJoint.angleHi.x, pmxJoint.angleHi.y, pmxJoint.angleHi.z));
			this->constraint = std::move(constraint);
			break;
		}
		case 2: {
			//P2P
			btVector3 p;
			p.setValue(pmxJoint.position.x, pmxJoint.position.y, pmxJoint.position.z);
			btVector3 pA = invAr * p;
			btVector3 pB = invBr * p;
			auto constraint = std::make_unique<btPoint2PointConstraint>(*rigidBodyA->rigidBody, *rigidBodyB->rigidBody, pA, pB);
			this->constraint = std::move(constraint);
			break;
		}
		case 3: {
			//ConeTwist
			auto constraint = std::make_unique<btConeTwistConstraint>(*rigidBodyA->rigidBody, *rigidBodyB->rigidBody, invA, invB);
			constraint->setLimit(pmxJoint.angleLo.z, pmxJoint.angleLo.y, pmxJoint.angleLo.x, pmxJoint.springMove.x, pmxJoint.springMove.y, pmxJoint.springMove.z);
			constraint->setDamping(pmxJoint.moveLo.x);
			constraint->setFixThresh(pmxJoint.moveHi.x);
			bool motor = pmxJoint.moveLo.z;
			constraint->enableMotor(motor);
			if (motor) {
				constraint->setMaxMotorImpulse(pmxJoint.moveHi.z);
				btQuaternion bq;
				bq.setEulerZYX(pmxJoint.springRotate.z, pmxJoint.springRotate.y, pmxJoint.springRotate.x);
				constraint->setMotorTargetInConstraintSpace(bq);
			}
			this->constraint = std::move(constraint);
			break;
		}
		case 4: {
			//Slider
			auto constraint = std::make_unique<btSliderConstraint>(*rigidBodyA->rigidBody, *rigidBodyB->rigidBody, invA, invB, true);
			constraint->setLowerLinLimit(pmxJoint.moveLo.x);
			constraint->setUpperLinLimit(pmxJoint.moveHi.x);
			constraint->setLowerAngLimit(pmxJoint.angleLo.x);
			constraint->setUpperAngLimit(pmxJoint.angleHi.x);

			bool linmot = pmxJoint.springMove.x;
			constraint->setPoweredLinMotor(linmot);
			if (linmot) {
				constraint->setTargetLinMotorVelocity(pmxJoint.springMove.y);
				constraint->setMaxLinMotorForce(pmxJoint.springMove.z);
			}

			bool angmot = pmxJoint.springRotate.x;
			constraint->setPoweredAngMotor(angmot);
			if (angmot) {
				constraint->setTargetAngMotorVelocity(pmxJoint.springRotate.y);
				constraint->setMaxAngMotorForce(pmxJoint.springRotate.z);
			}
			this->constraint = std::move(constraint);
			break;
		}
		case 5: {
			//Hinge
			auto constraint = std::make_unique<btHingeConstraint>(*rigidBodyA->rigidBody, *rigidBodyB->rigidBody, invA, invB, true);
			constraint->setLimit(pmxJoint.angleLo.x, pmxJoint.angleHi.x, pmxJoint.springMove.x, pmxJoint.springMove.y, pmxJoint.springMove.z);
			bool motor = pmxJoint.springRotate.x;
			if (motor) {
				constraint->enableAngularMotor(motor, pmxJoint.springRotate.y, pmxJoint.springRotate.z);
			}
			this->constraint = std::move(constraint);
			break;
		}
		}
	}


	//物理エンジンに、モデルの持っている剛体・ジョイントをくっつける
	void PoseSolver::AttachPhysics()
	{
		if (physics == nullptr)
			return;

		//ジョイントが無効な剛体に繋がっていないかチェック
		//1つでも無効なジョイントがあった場合はこのモデルの物理対応は中止する
		for (int i = 0; auto&& j : pmx->joints) {
			int N = pmx->bodies.size();
			if (j.bodyA < 0 || j.bodyB < 0 || j.bodyA >= N || j.bodyB >= N) {
				ThrowIfFailed(E_INVALIDARG, std::format(L"invalid joint #{}:{}", i, j.name));
				return;
			}
			i++;
		}

		body.resize(pmx->bodies.size());
		for (int i = 0; i < pmx->bodies.size(); i++) {
			body[i] = new RigidBody(pmx, i, this);
			physics->AddRigidBody(body[i]);
		}

		joint.resize(pmx->joints.size());
		for (int i = 0; i < pmx->joints.size(); i++) {
			auto& j = pmx->joints[i];
			joint[i] = new Joint(pmx, i, body[j.bodyA], body[j.bodyB]);
			physics->AddJoint(joint[i]);
		}

		physics->AddNotifee(this);
	}

	//物理エンジンから切断
	void PoseSolver::DetachPhysics()
	{
		if (physics == nullptr)
			return;

		//BulletPhysicsのconstraintはrigidbodyへのポインタを持っているので解放する時はcostraint(joint)から解放しないとダメとのこと
		for (auto&& jt : joint) {
			physics->RemoveJoint(jt);
			delete jt;
		}

		for (auto&& bd : body) {
			physics->RemoveRigidBody(bd);
			delete bd;
		}

		physics->DeleteNotifee(this);
	}


	inline btVector3 ConVec(const XMFLOAT3& v)
	{
		btVector3 b;
		b.setValue(v.x,v.y,v.z);
		return b;
	}

	inline btTransform ConMat(const Matrix& m)
	{
		btTransform b;
		b.setFromOpenGLMatrix(&m._11);
		return b;
	}

	//物理エンジン更新開始
	void PoseSolver::OnUpdatePhysicsBegin(bool active)
	{
		for (int i = 0; auto bd : body) {
			if (pmx->version >= 2.10f) {
				//インパルスモーフ
				if (impulseReset[i]) {
					bd->rigidBody->setLinearVelocity(btVector3(0, 0, 0));
					bd->rigidBody->setAngularVelocity(btVector3(0, 0, 0));
					bd->rigidBody->setInterpolationLinearVelocity(btVector3(0, 0, 0));
					bd->rigidBody->setInterpolationAngularVelocity(btVector3(0, 0, 0));
					/* PMX仕様にはこれをやるらしき事が書いてあるけど引数がよくわからないので
					if (bd->bone) {
						auto tr = bd->bone->transform;
						bd->rigidBody->setInterpolationWorldTransform(ConMat(tr));
					}
					*/
					bd->rigidBody->clearForces();
				} else {
					//60fpsを基準にdtに応じて倍率を掛ける
					float scaleL = physics->dtime() * 60;
					float scaleA = scaleL;

					//～Impulseメソッドの引数の単位は[Ns]らしい。(MMD物理では長さの単位は10cmなので重力加速度は98である事に注意)
					bd->rigidBody->applyCentralImpulse(ConVec(impulseLinear[i] * scaleL));
					bd->rigidBody->applyTorqueImpulse(ConVec(impulseAngular[i] * scaleA));

					auto locM = bd->GetTransform();
					vec3 linloc = vec3::TransformNormal(impulseLinearLocal[i] * scaleL, locM);
					bd->rigidBody->applyCentralImpulse(ConVec(linloc));
					vec3 rotloc = vec3::TransformNormal(impulseAngularLocal[i] * scaleA, locM);
					bd->rigidBody->applyTorqueImpulse(ConVec(rotloc));
				}
			}

			bool a = active;
			//物理キーを反映
			if (bd->bone) {
				int ibone = bd->bone->iBone;
				if (ibone >= 0) {
					int initMode = pmx->bones[ibone].physicsMode;
					bool physkey = solvedPose.boneKeys[ibone].physics;
					a = physkey && active;
					bd->physKey = physkey;
				}
			}
			bd->SetActivation(a);
			i++;
		}
	}

//物理エンジン更新終了、ボーンに反映
void PoseSolver::OnUpdatePhysicsEnd()
{
	for (auto bd : body) {
		bd->ReflectGlobalTransform();
	}

	root->Propagate(root);
	FinishMatrices();
	
	//physPoseの作成...solvedPoseとの違いは物理による変形が加味されている点
	//物理による変形が行われた直後の状態は、全ボーンのワールド変換は解決されているが、ローカル変換(自分→親)はてきとうになっている
	//てきとうになっているローカル変換をここで計算する
	physPose = solvedPose;
	for (int i = 0;  auto && key : physPose.boneKeys) {
		//物理が有効なボーンについてのキーならキーフレームのデータを物理の結果から逆算する
		if (pmx->bones[i].IsPhysics && physPose.boneKeys[i].physics) {
			auto b = bones[i];
			Matrix toP = Matrix(b->transform) * Matrix(b->parent->transform).Invert();
			b->toParent = toP;

			Vector3 o = b->origin;
			b->localTranslation = Vector3::Transform(o, toP)-o;
			b->localRotation = Quaternion::CreateFromRotationMatrix(toP);

			key.rotation = reinterpret_cast<XMFLOAT4&>(b->localRotation);
			key.position = b->localTranslation;

			//***導出***
			//行列toPは[-o][R][t][o]に分解できる
			// 但し、oはボーンの回転中心、キーフレームに書きたいのは[R]相当のクォータニオンと、[t]相当の3Dベクトル

			//toP = [-o][R][t][o], toP = [R][t']と書くと
			//[R][t'] = [-o][R][t][o]
			//[o][R][t'] = [R][t][o] →　[o][R][t'][-o] = [R][t] → [t] = [R-1][o][R][t'][-o]
			//この左上3x3要素は[I]であるのは自明。下1行は o[R][t']-oで良い
		}
		i++;
	}
}

//物理エンジンリセット開始
void PoseSolver::OnResetPhysicsBegin()
{
	for (auto bd : body) {
		bd->SetActivation(false);
		bd->ResetTransform();
	}
}

//物理エンジンリセット終了、ボーンに反映
//ResetはSolve直後やAスタンス時に呼んで剛体の位置をボーンアニメの状態に合わせる＆剛体の運動量を0にするのが目的なので
//剛体の位置をボーンに反映させるための処理は必要ないはず
void PoseSolver::OnResetPhysicsEnd()
{
	//for (auto bd : body)
		//bd->ReflectGlobalTransform();

	//for (int i = 0; i < body.size(); i++)
	//	body[i]->CalcLocalTransform();

	for (auto bd : body)
		bd->Reset();

	//root->Propagate(root);
	//FinishMatrices();
}

//Aスタンスから最後にSolveした時のキーフレームの状態にジワジワと近づける事で剛体が貫通状態からスタートするのをある程度防ぐ
//ポーズによっては却って物理がおかしくなるので、framesをなるべく増やすのが良いと思われる
void PoseSolver::OnPrewarmPhysics(int frames, int totalFrames)
{
	//framesが0なら初期化＝終了状態を保存
	if (frames == 0) {
		m_goalT.resize(bones.size());
		m_goalQ.resize(bones.size());

		for (int i = 0; i < bones.size(); i++) {
			m_goalT[i] = bones[i]->localTranslation;
			m_goalQ[i] = bones[i]->localRotation;
		}
	}

	float t = frames / (float(totalFrames) - 1);

	for (int j = 0; j < bones.size(); j++) {
		Vector3 p = m_goalT[j] * t;
		Quaternion q = Quaternion::Slerp(Quaternion::Identity, m_goalQ[j], t);
		bones[j]->localTranslation = p;
		bones[j]->localRotation = q;
		bones[j]->toParent = oRtMatrix(bones[j]->origin, q, p);
		bones[j]->transformResolved = false;
	}


}


//RelaxJointから呼ばれる、可動域0のジョイントをasobi分だけ動くように緩和する
void AsobiJoint(btVector3& lo, btVector3& hi, const Vector3& asobi)
{
	Vector3 a = asobi;
	if (lo.x() == hi.x()) {
		lo.setX(lo.x() - a.x);
		hi.setX(hi.x() + a.x);
	}
	if (lo.y() == hi.y()) {
		lo.setY(lo.y() - a.y);
		hi.setY(hi.y() + a.y);
	}
	if (lo.z() == hi.z()) {
		lo.setZ(lo.z() - a.z);
		hi.setZ(hi.z() + a.z);
	}
}

void PoseSolver::RelaxJoint(int iJoint, const XMFLOAT3& linear, const XMFLOAT3& angular)
{
	auto pmxJoint = pmx->joints[iJoint];
	//バネ付き6DoF以外は未対応
	if (pmxJoint.kind != 0)
		return;

	auto constraint = (btGeneric6DofSpringConstraint*)(joint[iJoint]->constraint.get());

	if (!constraint)
		return;

	btVector3 lo = btVector3(pmxJoint.moveLo.x, pmxJoint.moveLo.y, pmxJoint.moveLo.z);
	btVector3 hi = btVector3(pmxJoint.moveHi.x, pmxJoint.moveHi.y, pmxJoint.moveHi.z);
	AsobiJoint(lo, hi, linear);

	constraint->setLinearLowerLimit(lo);
	constraint->setLinearUpperLimit(hi);

	lo = btVector3(pmxJoint.angleLo.x, pmxJoint.angleLo.y, pmxJoint.angleLo.z);
	hi = btVector3(pmxJoint.angleHi.x, pmxJoint.angleHi.y, pmxJoint.angleHi.z);
	AsobiJoint(lo, hi, angular);
	constraint->setAngularLowerLimit(lo);
	constraint->setAngularUpperLimit(hi);

}

void PoseSolver::RelaxJoints(const std::vector<std::wstring>& keywords, const XMFLOAT3& linear, const XMFLOAT3& angular)
{
	auto lowKeys = keywords;
	vector<wstring> jnames(joint.size());

	//キーワードリストを小文字化
	for (int i = 0; auto && k : keywords) {
		transform(k.begin(), k.end(), lowKeys[i].begin(), [](wchar_t c) {return tolower(c); });
		i++;
	}
	
	//ジョイント名を小文字化
	for (int i = 0; auto && j : jnames) {
		j = pmx->joints[i].name;
		transform(j.begin(), j.end(), j.begin(), [](wchar_t c) {return tolower(c); });
		i++;
	}

	for (int j = 0; auto && jname : jnames) {
		for (auto && key : lowKeys) {
			if (jname.find(key) != wstring::npos) {
				this->RelaxJoint(j, linear, angular);
				break;
			}
		}
		j++;
	}
}

void PoseSolver::PurgeExternalKeys(int purgedID)
{
	for (auto&& [iFrame, value] : extraKeys) {
		auto&& exp = value.exp;
		std::erase_if(exp, [&](VMDExParent& o) { return o.parentID == purgedID; });
	}
}



bool PoseSolver::AnySelected()
{
	for (auto&& m : motionKeys)
		for (auto&& k : m)
			if (k.second.selected)
				return true;

	for (auto&& m : morphKeys)
		for (auto&& k : m)
			if (k.second.selected)
				return true;

	for (auto&& k : extraKeys)
		if (k.second.selected)
			return true;

	return false;
}

void PoseSolver::Subset(KeySubset& subset, bool trim)
{
	subset = {};
	int frontF = 0x7FFFFFFF;	//書き込まれているキーの中で最も先頭のフレーム番号

	subset.motions.clear();
	for (int i = 0; auto && bone : motionKeys) {
		for (auto&& key : bone) {
			if (key.second.selected) {
				subset.motions.push_back({ key.second, pmx->bones[i].name});
				frontF = min(frontF, key.second.iFrame);
			}
		}
		i++;
	}


	subset.morphs.clear();
	for (int i = 0; auto && morph : morphKeys) {
		for (auto&& key : morph) {
			if (key.second.selected) {
				subset.morphs.push_back({ key.second, pmx->morphs[i].name });
				frontF = min(frontF, key.second.iFrame);
			}
		}
		i++;
	}
	subset.extras.clear();
	for (auto&& key : extraKeys)
		if (key.second.selected) {
			subset.extras.push_back(key.second);
			frontF = min(frontF, key.second.iFrame);
		}

	//必要なら先頭フレーム番号を0としてフレーム番号を減らす
	if (trim) {
		for (auto&& key : subset.motions)
			key.iFrame -= frontF;
		for (auto&& key : subset.morphs)
			key.iFrame -= frontF;
		for (auto&& key : subset.extras)
			key.iFrame -= frontF;
	}
}

void PoseSolver::Revset(const KeySubset& subset, int frame, KeyUndo& undo, KeyUndo& redo)
{
	undo = {};
	redo = {};

	auto lambda = [&](auto& names, auto& undosel, auto& redosel, auto& keys, const auto& subs, auto& undorev, auto& redorev, auto& dict) {
			undosel.resize(keys.size());
			redosel.resize(keys.size());

			for (const auto& sub : subs) {
				int f = frame + sub.iFrame;
				if (dict.contains(sub.name)) {
					int ib = dict[sub.name];
					if (keys[ib].contains(f)) {
						undorev.push_back({ keys[ib][f], names[ib].name});	//上書前の情報保存
					} else {
						undosel[ib][f] = true;	//上書前の情報が無い場合、undo時に削除するフラグを立てる
					}
					auto key = sub;
					key.iFrame = f;
					redorev.push_back(key);	//リドゥするための情報
				}
			}
		};

	lambda(pmx->bones, undo.motionSel, redo.motionSel, motionKeys, subset.motions, undo.rev.motions, redo.rev.motions, boneDict);
	lambda(pmx->morphs, undo.morphSel, redo.morphSel, morphKeys, subset.morphs, undo.rev.morphs, redo.rev.morphs, morphDict);

	//表示・IK・外親
	for (const auto& sub : subset.extras) {
		int f = frame + sub.iFrame;
		if (extraKeys.contains(f)) {
			undo.rev.extras.push_back(extraKeys[f]);
		} else {
			undo.extraSel[f] = true;
		}
		auto key = sub;
		key.iFrame = f;
		redo.rev.extras.push_back(key);
	}

}

void PoseSolver::Undo(const KeyUndo& undo)
{
	//keysの中でselに挙げられているキーを削除する
	auto lambda = [](const auto& sel, auto& keys) { for (const auto& flag : sel) if (flag.second) keys.erase(flag.first); };

	//まず、新規に作成されたキーを削除する
	for (int i = 0; const auto & umaps : undo.motionSel) {
		lambda(umaps, motionKeys[i]);
		i++;
	}

	for (int i = 0; const auto & umaps : undo.morphSel) {
		lambda(umaps, morphKeys[i]);
		i++;
	}

	lambda(undo.extraSel, extraKeys);

	//次に、revに入っている「前の内容のキー」に上書する
	Join(undo.rev, 0);
}



void PoseSolver::Revdelset(bool except0f, KeyUndo& undo, KeyUndo& redo)
{
	undo = {};
	redo = {};

	//選択されたキーの内容をrevに保存する
	auto undoLambda = [](auto& keys, auto& rev, auto& name) { for (auto&& key : keys) if (key.second.selected) rev.push_back({ key.second, name}); };
	auto undoLambda2 = [](auto& keys, auto& rev) { for (auto&& key : keys) if (key.second.selected) rev.push_back({ key.second }); };

	for (int i = 0; auto && keys : motionKeys) {
		undoLambda(keys, undo.rev.motions, pmx->bones[i].name);
		i++;
	}

	for (int i = 0; auto && keys : morphKeys) {
		undoLambda(keys, undo.rev.morphs, pmx->morphs[i].name);
		i++;
	}

	undoLambda2(extraKeys, undo.rev.extras);


	//選択されたキーを削除対象とする。(except0fがtrueかつフレーム0のキーの場合はdefkeyをrevに保存する)
	auto redoLambda = [&](auto& keys, auto& rev, auto& sel, const auto& defkey) {
		for (auto&& key : keys) {
			if (key.second.selected) {
				if (key.first == 0 && except0f) {
					rev.push_back(defkey);
				} else {
					sel[key.first] = true;
				}
			}
		}
		};

	redo.motionSel.resize(motionKeys.size());
	for (int i = 0;  auto && item : motionKeys) {
		VMDMotion k0 = {};
		if (except0f) {
			k0.name = pmx->bones[i].name;
			k0.selected = true;
		}
		redoLambda(item, redo.rev.motions, redo.motionSel[i], k0);
		i++;
	}

	redo.morphSel.resize(morphKeys.size());
	for (int i = 0; auto && item : morphKeys) {
		VMDMorph k0 = {};
		if (except0f) {
			k0.name = pmx->morphs[i].name;
			k0.selected = true;
			k0.value = m_morphDefaults[i];
		}
		redoLambda(item, redo.rev.morphs, redo.morphSel[i], k0);
		i++;
	}

	VMDExtra e0 = {};
	for (auto ikb : IK2Bone) {
		e0.IK.push_back({ pmx->bones[ikb].name, true });
	}
	redoLambda(extraKeys, redo.rev.extras, redo.extraSel, e0);
}




void PoseSolver::Join(const KeySubset& subset, int frame)
{
	//ボーン名→ボーン番号辞書
	for (auto&& _key : subset.motions) {
		auto key = _key;
		key.iFrame += frame;
		//このモデルに含まれないボーン名の場合、読み込まない
		if (boneDict.contains(key.name)) {
			int iBone = boneDict[key.name];
			motionKeys[iBone][key.iFrame] = key;
		}
	}

	//Extras
	bool same;
	VMDExtra prevExtra, dmy;
	FindFrame<VMDExtra>(extraKeys, frame-1, prevExtra, dmy, same);	//読み込み位置の直前のextraキーの情報を得る
	for (auto&& _key : subset.extras) {
		auto key = prevExtra;		//IKについては読み込み位置の直前のextraキーの状態を踏襲する
		key.iFrame = _key.iFrame + frame;
		key.show = _key.show;
		key.selected = _key.selected;
		key.exp.clear();
		//追加されるサブセットキーの外親情報
		for (auto&& e : _key.exp) {
			VMDExParent de;
			//追加したい子ボーンはあるか？
			if (boneDict.contains(e.boneName)) {
				de.bone = boneDict[e.boneName];
				de.boneName = e.boneName;
				//IDが同じ外親モデルがあるか？
				if (externalSolver.contains(e.parentID)) {
					de.parentID = e.parentID;
					auto& exdict = externalSolver[e.parentID]->boneDict;
					//外親ボーン名の検索
					if (exdict.contains(e.parentBoneName)) {
						de.parentBone = exdict[e.parentBoneName];
						de.parentBoneName = e.parentBoneName;
						//以上3つが全部有れば外親キーとして登録
						key.exp.emplace_back(de);
					}
				}
			}
		}

		//追加されるサブセットキーに同名のIKボーンが含まれる場合はサブセットキーの情報で上書
		key.IK.resize(IK2Bone.size());
		for (auto&& _ik : _key.IK) {
			if (boneDict.contains(_ik.name)) {
				int ib = boneDict[_ik.name];
				int iik = Bone2IK[ib];
				if (iik >= 0) {
					key.IK[iik].name = _ik.name;	//ver1.20 追加(今まで忘れてたので)
					key.IK[iik].enabled = _ik.enabled;
				}
			}
		}
		extraKeys[key.iFrame] = key;
	}

	//モーフ名→モーフ番号辞書
	for (auto&& _key : subset.morphs) {
		auto key = _key;
		key.iFrame += frame;
		if (morphDict.contains(key.name)) {
			morphKeys[morphDict[key.name]][key.iFrame] = key;
		}
	}

}

void PoseSolver::SelectAllKeys(bool select)
{
	for (auto&& m : motionKeys)
		for (auto&& key : m)
			key.second.selected = select;

	for (auto&& m : morphKeys)
		for (auto&& key : m)
			key.second.selected = select;

	for (auto&& key : extraKeys)
		key.second.selected = select;
}


//最後にキーの打ってあるフレーム番号を得る
int PoseSolver::LastFrame()
{
	int last = 0;

	for (auto&& item : motionKeys)
		if (!item.empty())
			last = max(last, item.rbegin()->first);

	for (auto&& item : morphKeys)
		if (!item.empty())
			last = max(last, item.rbegin()->first);
	
	if (!extraKeys.empty())
		last = max(last, extraKeys.rbegin()->first);

	return last;
}

//subsetを左右反転する。当初はPoseSolverでないと出来ないとかと思ったがsubsetだけ注目すれば出来ると判明した
void PoseSolver::LRset(KeySubset& subset)
{
	//ボーンキー以外はクリア
	subset.morphs.clear();
	subset.extras.clear();

	for (auto&& item : subset.motions) {
		if (item.name.find(L'左') == 0) {
			item.name[0] = L'右';
		} else if (item.name.find(L'右') == 0) {
			item.name[0] = L'左';
		}
		item.position.x = -item.position.x;
		item.rotation = item.rotation * vec4(1, -1, -1, 1);
	}
}

bool PoseSolver::SetDefaultMorphValue(const std::wstring& morphName, float defaultValue, bool set0f)
{
	if (!morphDict.contains(morphName))
		return false;

	auto idx = morphDict[morphName];
	m_morphDefaults[idx] = defaultValue;
	if (set0f) {
		morphKeys[idx][0].value = defaultValue;
	}
	return true;
}


/***********************************************************************************
* CameraSolver
***********************************************************************************/
CameraSolver::CameraSolver(VMD* vmd) 
{
	type = SolverType::camera;

	if (vmd != nullptr) {
		//vmdから読み込み
		for (auto&& item : vmd->cameraKeys)
			cameraKeys.emplace_hint(cameraKeys.end(), item.iFrame, item);
		for (auto&& item : vmd->lightKeys)
			lightKeys.emplace_hint(lightKeys.end(), item.iFrame, item);
		for (auto&& item : vmd->selfShadowKeys)
			selfShadowKeys.emplace_hint(selfShadowKeys.end(), item.iFrame, item);
		//重力キーはvmdに入ってない
	}

	//0フレームにキーが無い場合はデフォルトキーを入れる
	if (cameraKeys.empty())
		cameraKeys[0] = {};
	
	if (lightKeys.empty())
		lightKeys[0] = {};
	
	if (selfShadowKeys.empty())
		selfShadowKeys[0] = {};

	if (gravityKeys.empty())
		gravityKeys[0] = {};
}

void CameraSolver::Matrices(const VMDCamera& cam, XMMATRIX& view, XMMATRIX& proj, XMFLOAT3& pos, float aspect, float nearZ, float farZ)
{
	//まず、カメラ→ワールドの回転変換行列を作る
	Matrix camRot = Matrix::CreateFromYawPitchRoll(-Vector3(cam.rotation));
	//カメラのZ軸の向きはワールド座標だとどの向きになる？
	Vector3 camZ = Vector3(camRot._31, camRot._32, camRot._33);
	//カメラのワールド座標を得る
	Vector3 p = camZ * cam.distance + cam.target;
	//ワールド座標→カメラ座標の変換を得る
	Matrix _view = camRot.Transpose();
	_view._41 = -p.Dot(camRot.Right());
	_view._42 = -p.Dot(camRot.Up());
	_view._43 = -p.Dot(camRot.Backward());
	view = _view.Transpose();
	pos = p;

	if (cam.parth) {
		//プロジェクション行列はいつも通りでヨシ
		proj = DirectX::XMMatrixTranspose(DirectX::XMMatrixPerspectiveFovLH(cam.viewAngle / 180 * PI, aspect, nearZ, farZ));
	} else {
		proj = DirectX::XMMatrixTranspose(DirectX::XMMatrixOrthographicLH(-cam.distance, -cam.distance/aspect, nearZ, farZ));
	}
}


void CameraSolver::TrackingMatrices(const XMMATRIX& bone, const XMFLOAT3& boneOrigin, const VMDCamera& cam, XMMATRIX& view, XMMATRIX& proj, XMFLOAT3& pos, float aspect, float nearZ, float farZ)
{
	Matrix tr = Matrix::CreateTranslation(cam.target + boneOrigin);
	Matrix rot = Matrix::CreateFromYawPitchRoll(-Vector3(cam.rotation));
	
	Matrix total = tr * Matrix(bone) * rot;
	pos = _41_42_43(total);
	view = total.Invert();
	view = Matrix(view).Transpose();

	//プロジェクション行列はいつも通りでヨシ
	if (cam.parth) {
		proj = DirectX::XMMatrixTranspose(DirectX::XMMatrixPerspectiveFovLH(cam.viewAngle / 180 * PI, aspect, nearZ, farZ));
	} else {
		proj = DirectX::XMMatrixTranspose(DirectX::XMMatrixOrthographicLH(-cam.distance, -cam.distance/aspect, nearZ, farZ));
	}
}

void CameraSolver::Solve(float time)
{
	float f = time;
	int iFrame = floorf(time);

	//カメラ
	if (cameraKeys.empty()) {
		camera = VMDCamera();
	} else {
		bool same;
		VMDCamera t0, t1;
		FindFrame<VMDCamera>(cameraKeys, iFrame, t0, t1, same);
		if ((t1.iFrame - t0.iFrame) <= 1) {
			//カメラとライトについては隣り合ったフレーム同士でキーが打たれている場合は補間しない
			//(モーションでもそれやるとストロボ状態になるのでカメラとライトだけにすべし)
			camera = t0;
			interpolated = false;
		} else {
			interpolated = true;

			//ベジェパラメータは純粋にt1の物なのでコピーしとく
			memcpy(camera.bezierParams, t1.bezierParams, sizeof(camera.bezierParams));;

			float t = (float)(f - t0.iFrame) / (float)(t1.iFrame - t0.iFrame);

			//注目点位置
			if (vec3(t0.target) == t1.target) {
				camera.target = t0.target;
			} else {
				vec3 bt;
				//bz.X[] は ax bx ay byの順
				bt.x = BezierT(t, t1.bezier.X[0], t1.bezier.X[1], t1.bezier.X[2], t1.bezier.X[3]);
				bt.y = BezierT(t, t1.bezier.Y[0], t1.bezier.Y[1], t1.bezier.Y[2], t1.bezier.Y[3]);
				bt.z = BezierT(t, t1.bezier.Z[0], t1.bezier.Z[1], t1.bezier.Z[2], t1.bezier.Z[3]);
				camera.target = Lerp(t0.target, t1.target, bt);
			}

			//回転
			if (vec3(t0.rotation) == t1.rotation) {
				camera.rotation = t0.rotation;
			} else {
				float bt = BezierT(t, t1.bezier.R[0], t1.bezier.R[1], t1.bezier.R[2], t1.bezier.R[3]);
				camera.rotation = Lerp(t0.rotation, t1.rotation, vec3(bt, bt, bt));
			}

			//距離
			if (t0.distance == t1.distance) {
				camera.distance = t0.distance;
			} else {
				float bt = BezierT(t, t1.bezier.L[0], t1.bezier.L[1], t1.bezier.L[2], t1.bezier.L[3]);
				camera.distance = Lerp(t0.distance, t1.distance, bt);
			}

			//画角
			if (t0.viewAngle == t1.viewAngle) {
				camera.viewAngle = t0.viewAngle;
			} else {
				float bt = BezierT(t, t1.bezier.V[0], t1.bezier.V[1], t1.bezier.V[2], t1.bezier.V[3]);
				camera.viewAngle = Lerp(t0.viewAngle, t1.viewAngle, bt);
			}

			camera.parentID = t0.parentID;
			camera.parentBone = t0.parentBone;
			camera.parth = t0.parth;
		}
	}

	//ライト
	if (lightKeys.empty()) {
		light = VMDLight();
	} else {
		bool same;
		VMDLight L0, L1;
		FindFrame<VMDLight>(lightKeys, iFrame, L0, L1, same);
		if ((L1.iFrame - L0.iFrame) <= 1) {
			light = L0;
		} else {
			float t = (float)(f - L0.iFrame) / (float)(L1.iFrame - L0.iFrame);
			vec3 ttt = vec3(t, t, t);
			light.color = Lerp(L0.color, L1.color, ttt);
			light.direction = Lerp(L0.direction, L1.direction, ttt);
		}
	}

	//セルフシャドウ
	if (selfShadowKeys.empty()) {
		selfshadow = VMDSelfShadow();
	} else {
		bool same;
		VMDSelfShadow t0, t1;
		FindFrame<VMDSelfShadow>(selfShadowKeys, iFrame, t0, t1, same);
		selfshadow = t0;
	}

	//重力
	if (gravityKeys.empty()) {
		gravity = VMDGravity();
	} else {
		bool same;
		VMDGravity t0, t1;
		FindFrame<VMDGravity>(gravityKeys, iFrame, t0, t1, same);
		if ((t1.iFrame - t0.iFrame) <= 1) {
			gravity = t0;
		} else {
			float t = (float)(f - t0.iFrame) / (float)(t1.iFrame - t0.iFrame);
			vec3 ttt = vec3(t, t, t);
			gravity.g = Lerp(t0.g, t1.g, t);
			gravity.direction = Lerp(t0.direction, t1.direction, ttt);
			gravity.noiseAmp = Lerp(t0.noiseAmp, t1.noiseAmp, t);
			gravity.noiseFreq = t0.noiseFreq;
		}
	}

	/* GPUに送るデータの作成、行列は転置済みの状態にする */
	Matrices(camera, viewMatrix, projectionMatrix, cameraPosition, aspect, nearZ, farZ);

	//Bullet用の重力ベクトル
	btGravity = gravity.BtGravity(time);

}

bool CameraSolver::AnySelected()
{
	for (auto&& k : cameraKeys)
		if (k.second.selected)
			return true;

	for (auto&& k : lightKeys)
		if (k.second.selected)
			return true;

	for (auto&& k : selfShadowKeys)
		if (k.second.selected)
			return true;

	for (auto&& k : gravityKeys)
		if (k.second.selected)
			return true;

	return false;
}

void CameraSolver::Subset(KeySubset& subset, bool trim)
{
	subset = {};

	int frontF = 0x7FFFFFFF;	//書き込まれているキーの中で最も先頭のフレーム番号

	auto lambda = [&](auto& sub, auto& keys) {
		sub.clear();
		for (auto&& key : keys)
			if (key.second.selected) {
				sub.push_back(key.second);
				frontF = min(frontF, key.second.iFrame);
			}
	};

	lambda(subset.cameras, cameraKeys);
	lambda(subset.lights, lightKeys);
	lambda(subset.selfShadows, selfShadowKeys);
	lambda(subset.gravities, gravityKeys);

	//必要なら先頭フレーム番号を0としてフレーム番号を減らす
	if (trim) {
		for (auto&& key : subset.cameras)
			key.iFrame -= frontF;
		for (auto&& key : subset.lights)
			key.iFrame -= frontF;
		for (auto&& key : subset.selfShadows)
			key.iFrame -= frontF;
		for (auto&& key : subset.gravities)
			key.iFrame -= frontF;
	}
}

void CameraSolver::Revset(const KeySubset& subset, int frame, KeyUndo& undo, KeyUndo& redo)
{
	undo = {};
	redo = {};

	auto lambda = [&](const auto& subs, auto& keys, auto& undosel, auto& undorev, auto& redorev) {
		for (const auto& sub : subs) {
			int f = frame + sub.iFrame;
			if (keys.contains(f)) {
				undorev.push_back(keys[f]);
			} else {
				undosel[f] = true;
			}
			auto key = sub;
			key.iFrame = f;
			redorev.push_back(key);
		}
		};

	lambda(subset.cameras, cameraKeys, undo.cameraSel, undo.rev.cameras, redo.rev.cameras);
	lambda(subset.lights, lightKeys, undo.lightSel, undo.rev.lights, redo.rev.lights);
	lambda(subset.selfShadows, selfShadowKeys, undo.selfShadowSel, undo.rev.selfShadows, redo.rev.selfShadows);
	lambda(subset.gravities, gravityKeys, undo.gravitySel, undo.rev.gravities, redo.rev.gravities);
};

void CameraSolver::Undo(const KeyUndo& undo)
{
	//keysの中でselに挙げられているキーを削除する
	auto lambda = [](const auto& sel, auto& keys) { for (const auto& flag : sel) if (flag.second) keys.erase(flag.first); };

	lambda(undo.cameraSel, cameraKeys);
	lambda(undo.lightSel, lightKeys);
	lambda(undo.selfShadowSel, selfShadowKeys);
	lambda(undo.gravitySel, gravityKeys);

	//次に、revに入っている「前の内容のキー」に上書する
	Join(undo.rev, 0);
}


void CameraSolver::Join(const KeySubset& subset, int frame)
{
	for (auto&& key : subset.cameras) 
		cameraKeys[key.iFrame] = key;

	for (auto&& key : subset.lights)
		lightKeys[key.iFrame] = key;

	for (auto&& key : subset.selfShadows)
		selfShadowKeys[key.iFrame] = key;

	for (auto&& key : subset.gravities)
		gravityKeys[key.iFrame] = key;
}


void CameraSolver::Revdelset(bool except0f, KeyUndo& undo, KeyUndo& redo)
{
	undo = {};
	redo = {};

	//選択されたキーの内容をrevに保存する
	auto undoLambda = [](auto& keys, auto& rev) { for (auto&& key : keys) if (key.second.selected) rev.push_back(key.second); };

	undoLambda(cameraKeys, undo.rev.cameras);
	undoLambda(lightKeys, undo.rev.lights);
	undoLambda(selfShadowKeys, undo.rev.selfShadows);
	undoLambda(gravityKeys, undo.rev.gravities);


	//選択されたキーを削除対象とする。(except0fがtrueかつフレーム0のキーの場合はdefkeyをrevに保存する)
	auto redoLambda = [&](auto& keys, auto& rev, auto& sel, const auto& defkey) {
		for (auto&& key : keys) {
			if (key.second.selected) {
				if (key.first == 0 && except0f) {
					rev.push_back(defkey);
				} else {
					sel[key.first] = true;
				}
			}
		}
		};

	redoLambda(cameraKeys, redo.rev.cameras, redo.cameraSel, VMDCamera());
	redoLambda(lightKeys, redo.rev.lights, redo.lightSel, VMDLight());
	redoLambda(selfShadowKeys, redo.rev.selfShadows, redo.selfShadowSel, VMDSelfShadow());
	redoLambda(gravityKeys, redo.rev.gravities, redo.gravitySel, VMDGravity());
}

void CameraSolver::SelectAllKeys(bool select)
{
	for (auto&& key : cameraKeys)
		key.second.selected = select;
	for (auto&& key : lightKeys)
		key.second.selected = select;
	for (auto&& key : selfShadowKeys)
		key.second.selected = select;
	for (auto&& key : gravityKeys)
		key.second.selected = select;

}

int CameraSolver::LastFrame()
{
	int ret = cameraKeys.rbegin()->first;
	ret = max(ret, lightKeys.rbegin()->first);
	ret = max(ret, selfShadowKeys.rbegin()->first);
	ret = max(ret, gravityKeys.rbegin()->first);
	return ret;
}

void CameraSolver::PurgeExternalKeys(int purgedID)
{
	for (auto&& [iFrame, value] : cameraKeys) {
		if (value.parentID == purgedID) {
			value.parentID = -1;
			value.parentBone = -1;
		}
	}
}

btVector3 VMDGravity::BtGravity(float frame)
{
	float n = 0;
	if (noiseAmp > 0)
		n = (FBM(frame / 30.0f, noiseFreq) * 2 - 1) * noiseAmp;	//本家とは違い逆方向に力が働くのも許容する(おそらく本家ではノイズの影響度は0～1)
	vec3 G = Normalize(direction) * (n + g);
	return btVector3(G.x, G.y, G.z) * 10;
}


//VPD読み込み
void KeySubset::LoadVPD(const std::wstring& filename, UINT codepage)
{
	*this = {};

	//strを, ; で区切る
	auto splitLambda = [](auto str) {
		std::string t;
		std::vector<std::string>ret;
		for (char c : str) {
			if (c == ',' || c == ';') {
				ret.push_back(t);
				t.clear();
			} else {
				t.push_back(c);
			}
		}
		if (!t.empty())
			ret.push_back(t);
		return ret;
	};

	//文字配列aのうち先頭count個の要素からfloat型のベクタを作る
	auto vecLambda = [](auto a, int count) {
		std::vector<float> v;
		for (int i = 0; i < count; i++) {
			v.push_back(std::stof(a[i]));
		}
		return v;
		};

	// 左右の空白文字を取り除く
	auto trimLambda = [](const std::string& str) {
		size_t start = 0;
		size_t end = str.size();

		// 左側の空白文字を取り除く
		while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
			++start;
		}

		// 右側の空白文字を取り除く
		while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
			--end;
		}

		return str.substr(start, end - start);
		};

	std::ifstream ifs(filename, std::ios::binary);

	if (!ifs)
		ThrowIfFailed(E_INVALIDARG, L"can't open vpd file {}", filename);

	std::string line;
	std::getline(ifs, line);
	if (line.find("Vocaloid Pose Data file") == std::string::npos)
		ThrowIfFailed(E_INVALIDARG, L"{} is not vpd file", filename);

	while (!ifs.eof()) {
		std::getline(ifs, line);
		if (line.find("Bone") == 0) {
			auto idx = line.find('{');
			if (idx == std::string::npos || idx >= line.size() - 1) {
				continue;
			}

			VMDMotion m = {};
			auto name = strTowstr(trimLambda(line.substr(idx + 1)), codepage);

			//tr
			std::getline(ifs, line);
			auto ns = splitLambda(line);
			if (ns.size() < 3)
				continue;
			auto v = vecLambda(ns, 3);

			//rot
			std::getline(ifs, line);
			ns = splitLambda(line);
			if (ns.size() < 4)
				continue;
			auto q = vecLambda(ns, 4);

			m.iFrame = 0;
			m.name = name;
			m.position = { v[0],v[1], v[2] };
			m.rotation = { q[0], q[1],q[2],q[3] };
			m.selected = true;
			motions.push_back(m);
		}
	}
}

//VMDオブジェクトを作る
VMD KeySubset::MakeVMD(const std::wstring& modelname) const
{
	VMD vmd = {};

	vmd.cameraKeys = cameras;
	vmd.extraKeys = extras;
	vmd.morphKeys = morphs;
	vmd.motionKeys = motions;
	vmd.selfShadowKeys = selfShadows;
	vmd.lightKeys = lights;
	vmd.modelName = modelname;

	return vmd;
}

//外部親情報のリターゲット
//①after.parentID/parentBone/boneのいずれかが0未満である場合、parentIDが一致しているキーを削除する
//②そうでない場合、beforeのparentID, parentBoneName, boneNameが全部一致している場合、afterのキーに置換する
void KeySubset::RetargetExternalKeys(const VMDExParent& before, const VMDExParent& after)
{
	if (after.parentID < 0 || after.parentBone < 0 || after.bone < 0) {
		for (auto&& key : extras) {
			if (key.selected)
				std::erase_if(key.exp, [&](const VMDExParent& e) { return e.parentID == before.parentID; });
		}
	} else {
		for (auto&& key : extras) {
			if (key.selected) {
				for (auto&& exp : key.exp) {
					if (exp.parentID == before.parentID && exp.parentBoneName == before.parentBoneName && exp.boneName == before.boneName)
						exp = after;
				}
			}
		}
	}
}

bool KeySubset::HasExternalKeys() const
{
	for (const auto& key : extras)
		if (!key.exp.empty())
			return true;
	return false;
}


// 軸に対応した回転角スカラーを抽出する（XYZ順の場合）
inline float ProjectEulerToAxis(const vec3& angles, const vec3& axis) {
	if (axis.x == 1.0f) return angles.x;
	if (axis.y == 1.0f) return angles.y;
	if (axis.z == 1.0f) return angles.z;
	// 任意軸への射影（任意軸が使われる場合のみ）
	return Dot(angles, Normalize(axis));
}

//IKリンクボーンから得られるIK計算用情報
IKLinkJoint::IKLinkJoint(const PMXIKLink& l)
{
	if (!l.isLimit)
		return;

	limit = true;
	//ロックされている軸を挙げる
	bool locX = l.hi.x == 0 && l.low.x == 0;
	bool locY = l.hi.y == 0 && l.low.y == 0;
	bool locZ = l.hi.z == 0 && l.low.z == 0;

	if (locX && locY && locZ) {
		fixed = true;
		hinge = false;
		return;
	} else if (locX && locY) {
		hingeAxis = vec3(0,0,1);
		hinge = true;
	} else if (locY && locZ) {
		hingeAxis = vec3(1, 0, 0 );
		hinge = true;
	} else if (locX && locZ) {
		hingeAxis = vec3( 0,1,0 );
		hinge = true;
	}
	hi = l.hi;
	lo = l.low;
	hingeHi = ProjectEulerToAxis(hi, hingeAxis);
	hingeLo = ProjectEulerToAxis(lo, hingeAxis);
}

//ローカル回転qを関節の可動範囲から推測される良さげな所に持っていく
Quaternion IKLinkJoint::Limit(const Quaternion& q) const
{
	if (!limit)
		return q;

	//qを軸hax周りに限定する。角度についての制限については以下に詳述
	auto hingeLambda = [&](const vec3& hax, float l, float h) {
		float a = AngleAroundAxis(q, hax);	//a:qによるh周りの回転量[rad]
		auto [b, d] = ClampAngle(a, l, h);	//b:aを[l..h]の範囲でクランプした物;  d:aと[l..h]の範囲までの円周上の距離
		if (d) {
			//制限範囲内に納まってなかった場合、逆方向に回転させる
			//例えばひざIKで逆関節状態になってる場合、正反対に同じだけ回したら親関節が合わせに行くので良い感じの順関節になるはず、という理屈と思われる
			auto [b_rev, d_rev] = ClampAngle(-a, l, h);
			if (d_rev < d) {
				//逆方向で制限範囲に納まる、または逆方向の方が制限範囲により近いなら逆方向を採用
				b = b_rev;
			}
		}
		return vecQ::CreateFromAxisAngle(hax, b);
		};

	if (hinge) {
		//1軸ヒンジ
		return hingeLambda(hingeAxis, hingeLo, hingeHi);
	} else {
		//複合軸制限の場合、各軸の制限範囲から乗算順序を変える
		vecQ xq, yq, zq;
		xq = hingeLambda({ 1,0,0 }, lo.x, hi.x);
		yq = hingeLambda({ 0,1,0 }, lo.y, hi.y);
		zq = hingeLambda({ 0,0,1 }, lo.z, hi.z);

		constexpr float PSI = PI * 0.5f;
		if (lo.x > -PSI && hi.x < PSI) {
			return zq * xq * yq;
		} else if (lo.y > -PSI && hi.y < PSI) {
			return xq * yq * zq;
		} else {
			return yq * zq * xq;
		}
	}
}

} //namespace

