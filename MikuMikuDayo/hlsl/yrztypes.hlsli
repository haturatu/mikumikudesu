//YRZ型情報

#ifndef YRZTYPES_HLSLI
#define YRZTYPES_HLSLI

namespace YRZ {

//クォータニオン同士の積
float4 MulQ(float4 q, float4 p)
{
    return float4( cross(q.xyz,p.xyz) + q.w*p.xyz + p.w*q.xyz, dot(q.xyz,p.xyz)-q.w*p.w );
}

//デュアルクォータニオン
struct DualQ {
    float4 q;   //回転分
    float4 t;   //平行移動分
    DualQ operator + (DualQ d) { DualQ R = {q+d.q, t+d.t}; return R; }
    DualQ operator - (DualQ d) { DualQ R = {q-d.q, t-d.t}; return R; }
    DualQ operator * (DualQ d) { DualQ R = {MulQ(q,d.q), MulQ(t,d.q) + MulQ(q,d.t)}; return R; }
    DualQ operator * (float s) { DualQ R = {q*s,t*s}; return R; }
    DualQ operator / (float s) { DualQ R = {q/s,t/s}; return R; }
};

//skyboxを2次までのSHをした結果格納用
//c[0]がl=0, c[1-3]がl=1,m=-1,0,+1、c[4-8]がl=2,m=-2～+2 に対応
struct SHCoeff {
	float4 c[9];
};

//Walker箱の1要素
struct WalkersAlias {
    uint pair;     //ペアになる要素のインデックス。-1の時、ペア無し。
    //「この要素」を選択する確率
    // 値/平均値で「この要素」を選択する確率pが決まる
    // 確率1-pで「ペア要素」が選択される。ペア無しの場合valueによらず「この要素」が選択される
    float p;
    float pdf;  //「この要素」がサンプルされる確率(複数の要素に跨って入っている可能性があるので単純にpではない)
};

//Walker箱作成用、インデックスと値のペア
struct WalkersWorker {
    uint index;
    float value;
};



};

#endif