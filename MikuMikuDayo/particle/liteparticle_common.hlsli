//LiteParticle共通HLSL
static float Alpha = 1-_A;
static float4x4 InvCenterMatrix = InverseRTMatrix(CenterMatrix);
static int Amount = max(min((round((1-_Amount) * ParticleCount)),ParticleCount) * 4,0);   //表示に使う頂点の数
static float AnimSpeed = lerp(0.1,2,_AnimSpeed);
static float Arc = _Arc * PI;
static float Koshi = lerp(1,10,_Koshi);
static float EntireSize = exp2((_EntireSize-0.5)*10);

//ポストプロセス前の画像をポストプロセス後の画像としてそのままパスする
#ifdef YRZ_PASS_Copy
struct VSO { float4 position:SV_POSITION; float2 uv:TEXCOORD; };
VSO VS( float4 pos : POSITION, float2 uv:TEXCOORD ) { VSO vso = (VSO)0; vso.position = pos;  vso.uv = uv; return vso; }
float4 PS(VSO vso) : SV_TARGET
{
	return ScreenTexture.Sample(samp, vso.uv);
}
#endif


//パーティクルのメッシュ全体を出力するコンピュートシェーダ
#ifdef YRZ_PASS_Computer
YRZ_NUMTHREADS
void CS( uint3 id : SV_DispatchThreadID )
{
	uint i = id.x;

	//頂点インデクス
	//MMDモデルはTRIANGLELIST。3つの頂点インデクスで1つの三角形が定義される
	uint p = i*6;
	uint q = i*4;
	IBuf[p] = q;
	IBuf[p+1] = q+1;
	IBuf[p+2] = q+3;
	IBuf[p+3] = q+1;
	IBuf[p+4] = q+2;
	IBuf[p+5] = q+3;
}
#endif

//ユーティリティ

float dstep01(float a, float b, float x)
{
    return smoothstep(0,a,x) * smoothstep(1,b,x);
}

//ソフト処理付き奥行処理
float SoftParticle(VS_OUTPUT IN, float near = 0.5, float far = 5)
{
    float a = 1;
    float myDepth = IN.Pos.w;
    float depth = NormalDepth[IN.Pos.xy].w;
    if (depth) {
		a *= smoothstep(-near,-far, myDepth - depth);
    }
    return a;
}
