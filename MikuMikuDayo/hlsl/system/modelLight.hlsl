//モデルの各ポリゴンの面積からpdf,cdfを求めるシェーダ
//モデルごとに1回ずつ起動される

#include "../dayotypes.hlsli"
#include "../yrz.hlsli"

using namespace YRZ;
using namespace Dayo;


StructuredBuffer<Vertex> VB : register(t0);				//頂点バッファ
StructuredBuffer<uint> IB : register(t1);				//インデクスバッファ

RWStructuredBuffer<WalkersWorker>Work : register(u0);
RWStructuredBuffer<WalkersAlias>Walker : register(u1);
RWStructuredBuffer<float>Areas : register(u2);
RWStructuredBuffer<MaterialFace>Mat2Face : register(u3);           //マテリアル番号→最初の面番号と面数

//面ごとに1回起動される(id.xは面番号)
[numthreads(32,1,1)]
void ModelAreaCS( uint3 id:SV_DispatchThreadID )
{
    float3 p[3];
    for (int i=0; i<3; i++)
        p[i] = VB[IB[id.x*3+i]].position;

    Areas[id.x] = max(length(cross(p[0]-p[1],p[0]-p[2]))/2, 1e-10);  //面積0だとしても1e-10くらいは面積があるとして最低保証を入れる
}

//Walker's alias法の箱を材質数分だけ作るパス。マテリアルの数だけ起動される
[numthreads(1,1,1)]
void ModelLittleBigCS( uint3 id:SV_DispatchThreadID )
{
    uint i = id.x;
    uint s = Mat2Face[i].start;

    Mat2Face[i].totalArea = SetupWalkersAlias(Walker, Work, s, Mat2Face[i].count, Areas, s);
}

/*
//材質ごとに1回起動される(id.xは材質番号)
//材質毎の総面積を計算, MatSumは起動前に0クリアされている必要がある
[numthreads(32,1,1)]
void ModelAreaSumCS( uint3 id:SV_DispatchThreadID )
{
    uint idx = Mat2Face[id.x].start;
    uint n = Mat2Face[id.x].count;

    for (uint i=0; i<n; i++) {
        MatSum[id.x] += OutBuf[idx+i].area;
    }
}

//各面積を全面積で割ってpdfとする(id.xは面番号)
[numthreads(32,1,1)]
void ModelPDFCS( uint3 id:SV_DispatchThreadID )
{
    uint f = id.x;
    float t = MatSum[Face2Mat[id.x]];
    OutBuf[f].pdf = OutBuf[f].area / t;
    OutBuf[f].totalArea = t;
}

//cdf, 材質ごとに1回起動される(id.xは材質番号)
[numthreads(32,1,1)]
void ModelCDFCS( uint3 id:SV_DispatchThreadID )
{
    uint idx = Mat2Face[id.x].start;
    uint n = Mat2Face[id.x].count;

    if (n<=1) {
        OutBuf[idx].cdf = OutBuf[idx].pdf;
        return;
    }

    OutBuf[idx].cdf = OutBuf[idx].pdf;
    for (uint i=1; i<n-1; i++)
        OutBuf[i+idx].cdf = OutBuf[i+idx-1].cdf + OutBuf[i+idx].pdf;
    
    OutBuf[idx+n-1].cdf = 1;    //数値誤差を考えて。最後のインデクスには必ず1が入るようにする
}
*/

