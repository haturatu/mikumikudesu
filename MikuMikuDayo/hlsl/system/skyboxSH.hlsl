#include "../yrz.hlsli"
using namespace YRZ;

Texture2D<float4>Skybox : register(t0);			//ソース画像

//SH
RWStructuredBuffer<SHCoeff>SkyboxSHX : register(u0);	//各行をSHした結果 H要素
RWStructuredBuffer<SHCoeff>SkyboxSH : register(u1);	//↑を重みづけ平均したSH係数 1要素

//gibbs現象の抑止のため、輝度に制限を付ける(バイアスあり)
float4 sky(uint2 uv)
{
	float max_lum = 1e+6;
	float4 C = Skybox[uv];
	float L = RGBtoY(C.rgb);

	if (L<=max_lum)
		return C;
	else
		return float4(C.rgb * max_lum / L, C.a);
}

/* Y-up版。skyboxテクスチャの上方向をY+とする
void SHComboX( uint3 id:SV_DispatchThreadID )
{
	float W,H;
	Skybox.GetDimensions(W,H);
	float3 n;
	float t = (id.x+0.5) / H * PI;
	float ct,st;
	sincos(t, st,ct);
	n.y = ct;
	SHCoeff C = (SHCoeff)0;

	for (int x=0; x<W; x++) {
		float p = (x+0.5) / W * 2*PI;
		float sp,cp;
		sincos(p, sp,cp);
		n.z = -cp * st;
		n.x = -sp * st;
		uint2 uv = {x,id.x};
		C.c[0] += Skybox[uv] * 0.282095;
		C.c[1] += Skybox[uv] * -0.488603 * n.x;
		C.c[2] += Skybox[uv] * 0.488603 * n.y;
		C.c[3] += Skybox[uv] * -0.488603 * n.z;
		C.c[4] += Skybox[uv] * 1.092548 * n.x * n.z;
		C.c[5] += Skybox[uv] * -1.092548 * n.y * n.z;
		C.c[6] += Skybox[uv] * 0.315392 * (3 * n.y * n.y - 1);
		C.c[7] += Skybox[uv] * -1.092548 * n.x * n.y;
		C.c[8] += Skybox[uv] * 0.546274 * (n.x * n.x - n.z * n.z);
	}

	for (int i=0; i<9; i++) {
		C.c[i] /= W;
	}
	SkyboxSHX[id.x] = C;
}
*/

//Z-up 右手版、skyboxテクスチャの上方向をZ+とする
[numthreads(32,1,1)]
void SHComboX( uint3 id:SV_DispatchThreadID )
{
	float W,H;
	Skybox.GetDimensions(W,H);
	float3 n;
	float t = (id.x+0.5) / H * PI;
	float ct,st;
	sincos(t, st,ct);
	n.z = ct;
	SHCoeff C = (SHCoeff)0;

	//係数はPeter-Pike Sloan氏 "Stupid Spherical Harmonics (SH) Tricks"より引用
	for (int x=0; x<W; x++) {
		float p = (x+0.5) / W * 2*PI;
		float sp,cp;
		sincos(p, sp,cp);
		n.x = cp * st;
		n.y = sp * st;
		uint2 uv = {x,id.x};
		float4 s = sky(uv);
		C.c[0] += s * 0.282095;

		C.c[1] += s * -0.488603 * n.y;
		C.c[2] += s * 0.488603 * n.z;
		C.c[3] += s * -0.488603 * n.x;

		C.c[4] += s * 1.092548 * n.x * n.y;
		C.c[5] += s * -1.092548 * n.y * n.z;
		C.c[6] += s * 0.315392 * (3 * n.z * n.z - 1);
		C.c[7] += s * -1.092548 * n.x * n.z;
		C.c[8] += s * 0.546274 * (n.x * n.x - n.y * n.y);
	}

	for (int i=0; i<9; i++)
		C.c[i] /= W;

	SkyboxSHX[id.x] = C;
}


//SHの計算(縦方向:立体角で重みづけ平均)
[numthreads(1,1,1)]
void SHComboY( uint3 id:SV_DispatchThreadID )
{
	float W,H;
	Skybox.GetDimensions(W,H);

	SkyboxSH[0] = (SHCoeff)0;

	for (int i=0; i<H; i++) {
		float s = sin((i+0.5) / H * PI);
		for (int j=0; j<9; j++)
			SkyboxSH[0].c[j] += SkyboxSHX[i].c[j] * s;	//sで重みづけするので平均で2/π倍されている
	}

	for (int i=0; i<9; i++)
		SkyboxSH[0].c[i] *= 2 * PI * PI / H;	//2*PI*PI * (2/PI) = 4*PI
}