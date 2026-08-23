//「普通のスキニング」をするためのユーティリティ

namespace Dayo {

void DQS(inout Vertex v, Skinning s)
{
	//linear dual quadternion blending 版
	YRZ::DualQ qq = YRZ::DualQFromQQ(0,0);
	bool pivotInitialized = false;
	float4 pivot = 0;	//最初に見つかった有効なボーン行列の回転要素を「軸」とする
	for (int i=0; i<4; i++) {
		if ((s.iBone[i] >= 0 && s.weight[i])) {
			float w = s.weight[i];
			YRZ::DualQ pp = YRZ::DualQFromMatrix(BoneMatrix[s.iBone[i]]);
			if (!pivotInitialized) {
				pivot = pp.q;
				pivotInitialized = true;
			} else if (dot(pivot,pp.q) < 0) {	//「軸」から見て遠回りの回転の場合、近い方向の回転を選択する
				w = -w;
			}
			qq = YRZ::AddDualQ(qq, ScaleDualQ(pp,w));
		}
	}

	/* 近似度の低い版 普通に見てる分にはほぼ違いは判らない
	DualQ qq = DualQFromQQ(0,0);
	bool pivotInitialized = false;
	float4 pivot = 0;	//最初に見つかった有効なボーン行列の回転要素を「軸」とする
	float tw = 0;
	for (int i=0; i<4; i++) {
		if ((s.iBone[i] >= 0 && s.weight[i])) {
			float w = s.weight[i];
			DualQ pp = DualQFromMatrix(BoneMatrix[s.iBone[i]]);
			if (!pivotInitialized) {
				pivot = pp.q;
				pivotInitialized = true;
			} else if (dot(pivot,pp.q) < 0) {	//「軸」から見て遠回りの回転の場合、近い方向の回転を選択する
				pp = ScaleDualQ(pp,-1);
			}
			tw += w;
			qq = NormalizeDualQ(InterpolateDualQ(qq, pp, w/tw));
		}
	}
	*/

	float4x4 m = YRZ::MatrixFromDualQ(NormalizeDualQ(qq));
	v.position = mul(float4(v.position,1),m).xyz;
	v.normal = normalize(mul(v.normal,(float3x3)m));
}

//Linear Blend Skinning = BDEF1,2,4
void LBS(inout Vertex v, Skinning s, uniform int count = 4)
{
	float4x4 m = 0;
	for (int i=0; i<count; i++) {
		if (s.iBone[i] >= 0) {
			m += BoneMatrix[s.iBone[i]] * s.weight[i];
		}
	}
	v.position = YRZ::Transform(v.position,m);
	v.normal = normalize(YRZ::Rotate(v.normal,m));
	v.tangent = normalize(YRZ::Rotate(v.tangent,m));
}

#if 0
//SDEF実装に当たってのメモ
void _SDEF(inout Vertex v, Skinning s)
{
	float4x4 m0,m1;
	float w = s.weight[0];
	float3 c = s.sdef_c;

	/* cr0,cr1の算出はbenikabocha様 SABAのソースコードを参考にしました
	//https://github.com/benikabocha/saba/blob/master/src/Saba/Model/MMD/PMXModel.cpp
	//ランタイムの負荷を削減するために、SABAみたいにcr0,cr1まで読み込み時に計算しといた方がよさげ
	float3 rw = s.sdef_r0 * w + s.sdef_r1 * (1-w);	//このs.sdef_xxパラメータはpmxモデルに書いてある値そのまま
	float3 r0 = c + s.sdef_r0 - rw, r1 = c + s.sdef_r1 - rw;
	float3 cr0 = (c + r0)/2,  cr1 = (c + r1)/2; 
	↑のコードをよく見てみたところ
	r0 = c + (1-w)*(s.sdef_r0-s.sdef.r1); r1 = c - w * (s.sdef_r0-s.sdef_r1);
	つまり、pmxに格納されているsdef.r0とsdef.r1というのは互いの差(sdef.r0-sdef.r1)さえ所望の値なら、sdef.r0とsdef.r1自体の値は幾つでも良いので
	シェーダに渡す情報量を削減する場合はc と sdef.r0-sdef.r1だけでOK
	以上より、dR = (s.sdef_r0-s.sdef.r1)と置くと、cr0 = c + (1-w)*dR/2, cr1 = c - w*dR/2
	12バイト/頂点分メモリを削減できるので、cとdR/2 だけ入れとくのが良いでしょう */
	float3 hdr = (s.sdef_r0 - s.sdef_r1)/2;	//←をpmxデータの読み込み時に計算しとく
	float3 cr0 = c + (1-w) * hdr;	//cr0,cr1はここで計算する
	float3 cr1 = c - w * hdr;
	
	//以下のSDEF計算用のコードは したらば「MMD関連プログラミングスレ」を参考にしました
	//https://jbbs.shitaraba.net/bbs/read.cgi/music/23040/1285499541/472-474
	//↑のコードを元に一部改変
	m0 = BoneMatrix[s.iBone[0]];
	m1 = BoneMatrix[s.iBone[1]];

	//↓ここだけしたらばのコードとちょっと違うけど、これで動いてる気がする、ボーンの親子関係に非依存だし、いいんじゃないスか？
	float4x4 m = MatrixFromQuat(Slerp(QuatFromMatrix(m1),QuatFromMatrix(m0),w));

	/* おそらく、したらばのコードに書いてある事はこうなのではないか、という一応動作する下書き
	//平行移動分の計算は c+r0をm0,c+r1をm1で一次変換→ 補間 → 2で割って中点を得る
	//2行目以降のcとか-cのみの項は打ち消されてしまうので特に意味なしと思われるが幾何学的な意味を考える上では役立つ？
	float3 P = v.position;
	P = Rotate(P-c, m)
	+ (c
	+ (Transform(c,m0) - c + Transform(r0, m0)) * w
	+ (Transform(c,m1) - c + Transform(r1, m1)) * (1-w) ) * 0.5;
	*/

	//↑を整理、cとr0,r1の中点(cr0,cr1)を一次変換 → 補間する。とてもシンプル おそらく、10年の間に誰かがcr0,cr1を計算するようにして同様に整理したはず
	v.position = Rotate(v.position-c, m) + lerp( Transform(cr1,m1), Transform(cr0,m0), w );
	v.normal = Rotate(v.normal, m);

	/*その他注釈
	【cとcr0とcr1って結局何なのか？】
	BDEF(LBS)の場合、回転中心はボーンの原点oで固定されていて、頂点vはRotate(v-o,m) + Transform(o,m) としてグローバル変形行列mを使って変形される
	SDEFの場合、sdef_cが回転中心になるが、平行移動分を別にcr0,cr1として指定している
	これにより、回転しながらずれる、という変形が出来る
	具体的には、o周りの回転Rと平行移動tは、通常(v-o)R(o+t)で表されるが、oがsdef_cとなり、(o+t)がcr0～cr1と変化する
	・回転中心はsdef_cで固定されウェイト値によって変化しない
	・平行移動分はウェイト値によってcr0→cr1と変化する
	*/

}
#endif

//↑の整理後
void SDEF(inout Vertex v, Skinning s)
{
	float3 c = s.sdef_c;
	float w = s.weight.x;		//影響ボーンからのウェイトは必ず合計1になった状態でモデルは作られている、とみなす
	float3 hdr = s.weight.yzw;	//読み込み時に (sdef_r0 - sdef_r1)/2 を仕込んで置く
	float3 cr1 = c - w * hdr;	//cr0,cr1はここで計算する
	float3 cr0 = cr1 + hdr;
	float4x4 m0 = s.iBone[0]>=0 ? BoneMatrix[s.iBone[0]] : YRZ::Identity44;
	float4x4 m1 = s.iBone[1]>=0 ? BoneMatrix[s.iBone[1]] : YRZ::Identity44;
	float4x4 m = YRZ::MatrixFromQuat(YRZ::Slerp(YRZ::QuatFromMatrix(m1),YRZ::QuatFromMatrix(m0),w));
	v.position = YRZ::Rotate(v.position-c, m) + lerp( YRZ::Transform(cr1,m1), YRZ::Transform(cr0,m0), w );
	v.normal = YRZ::Rotate(v.normal, m);
	v.tangent = YRZ::Rotate(v.tangent, m);
}


//「普通のスキニング」を実行
Vertex DefaultSkinning(uint idx)
{
	Vertex v = VB[idx];//RemoveEx(VB[idx]);

	//頂点/UVモーフ
	MorphPointer mp = MorphTablePointer[idx];
	if (mp.where >= 0) {
		for (int i=0; i<mp.count; i++) {
			int tidx = i + mp.where;
			MorphItem mi = MorphTable[tidx];
			float value = MorphValues[mi.iMorph];
			int target = mi.target;
			if (mi.target == -1)
				v.position += mi.delta.xyz * value;
			else if (mi.target == 0)
				v.uv += mi.delta.xy * value;
			else if (mi.target <= 4)
				v.exuv[mi.target-1] += mi.delta * value;
		}
	}
	v.normal = normalize(v.normal);
	v.tangent = YRZ::ComputeTBN(v.normal)[0];

	//スキニング
	Skinning s = Skin[idx];

	//weightTypeによって補間方式の切り替え
	if (s.weightType == 3)
		SDEF(v,s);
	else if (s.weightType == 2)	//BDEF4
		LBS(v,s);
	else if (s.weightType == 1)	//BDEF2
		LBS(v,s,2);
	else if (s.weightType == 4) //QDEF(PMX2.1)
		DQS(v,s);
	else
		LBS(v,s,1);	//BDEF1

	return v;
}

};