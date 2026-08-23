
namespace Dayo {

// CPU側からConstantBufferに積まれる内容
//constant bufferに詰められた値は名前空間を貫通するので
//無名名前空間からでもアクセスできるとのこと
cbuffer ViewCB : register(b0)
{
	//カメラから導出される必須情報
	float4x4 ViewMatrix;			//ビュー行列
	float4x4 ProjectionMatrix;		//プロジェクション行列
	int Perspective;				//0:正射影 1:透視投影
	int CameraInterpolated;			//0:カメラのキーフレームは連続して打たれておりカメラはカットの切り替えなどでワープしている 1:カメラはスムーズに動いている
	//モデルの個数など
	uint ModelCount;			//モデルの個数
	uint TotalMaterialCount;	//全モデルでの材質数の合計(各材質の材質数はModel2Matを参照する)
	//時刻
	float Time;						//動作モードが「ガンガンいこうぜ」だった場合はRealTime、そうでないならFrameTimeと同じ
	float DTime;					//前回シェーダが実行されてからのTimeの差分
	float FrameTime;				//時刻[秒] = 再生中のフレーム番号/30
	float DFrameTime;				//前回シェーダが実行されてからのFrameTimeの差分
	float RealTime;					//実時刻
	float DRealTime;				//前回シェーダが実行されてから経過した実時刻
	int MouseDown;					//bit0:左 bit1:右 bit2:中ボタン それぞれの押下状態(true:1)
	int MouseClicked;				//bit0:左 bit1:右 bit2:中ボタン それぞれ、現在のフレームでクリックされたと判定されたか
	float2 MousePos;				//レンダー窓内でのマウスカーソルの位置、左上が0、右下が1
	int Playing;					//再生中・録画中なら1、編集中なら0
	int _CBpad0;
	//出力
	uint2 Resolution;				//出力先の解像度
	//レンダラによって扱いが変わる情報
	uint iSample;					//収束開始からのサンプル番号、0スタート
	uint SamplesPerFrame;			//1フレームあたりのサンプル数、出力時以外は1
	float3 LightColor;				//MMDの光源色(ガンマ色空間)
	int SelfShadowMode;				//セルフ影モード(0:無効、1:モード1、2:モード2)
	float3 LightDirection;			//MMDの光源の向き(正規化済み。パッチ→光源ではなく光線の進行方向であることに注意)
	float SelfShadowDistance;		//セルフ影距離(0～1、1 - UIでの値/10000 )
	float SceneRadius;				//シーン全体の半径(BDPT用)
	//Dayo内部のシステムシェーダから参照される情報
    int ScreenBMPMode;              //screen.bmpの扱い
	int BackgroundMode;				//背景の描画をどうするかモード 0:レンダラデフォルト 1:screen.bmp貼り付け 2:白 3:黒 4:チェッカー
	int BackgroundTransparent;		//背景は出力時にα=0とする
	int MaterialHighLight;			//選択中の材質をハイライト表示する?
	//デノイザ
	int DenoiserEnabled;			//OIDNによるデノイズを実行する(0:無効 1:有効)
	//イベント処理用
	int OnStart;					//Play,Recordが押されて最初に実行される時は1
	int OnLoadSkybox;				//skyboxが変更されたら1
	int OnResize;					//リサイズされてから最初に実行される場合は1
	int OnLoad;						//いずれかのエフェクトが読み込まれて最初に実行される時は1
};

//以上から導出される頻繁に使われそうな数値

//ワールド座標系でのカメラの右/上/前方向と位置
static float3 CameraRight = ViewMatrix._11_21_31;
static float3 CameraUp = ViewMatrix._12_22_32;
static float3 CameraForward = ViewMatrix._13_23_33;
static float3 CameraPosition = -mul((float3x3)ViewMatrix,ViewMatrix._41_42_43);
//垂直画角θに対するcot(θ/2)
static float CameraFoV = ProjectionMatrix._22;
//アスペクト比
static float AspectRatio = (float)Resolution.x/Resolution.y;
static float4x4 ViewProjectionMatrix = mul(ViewMatrix, ProjectionMatrix);

}; //namespace