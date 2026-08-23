//パストレーサ用補助パス群

//メインのレンダリング以外の、以下のパスを定義する
//クリア・アキュムレーション・OIDNへの入力作成


//HistoryにRTOutputとHistoryをブレンドした結果を入れてアキュムレーションをする
#ifdef YRZ_PASS_Acc
[shader("raygeneration")]
void Acc()
{
	uint2 p = DispatchRaysIndex().xy;
	float4 r = RTOutput[p];
	float4 c = CurrentFrame[p];
	RTOutput[p] = lerp(r,c,1.0/(iSample+1));
	RTOutput[p].a = saturate(RTOutput[p].a);	//α値は0～1の範囲にとどめる(念のため)
}
#endif

//GBufferとOIDNへの入力
#ifdef YRZ_PASS_GBuffer
struct PayloadOIDN
{
	int ID;
	int iFace;
	float3 Albedo;
	float3 Normal;
	float Tr;	//transmittance
	float t;	//RayTCurrent;
	float2 st;  //barycentrics;
};
[shader("raygeneration")]
void RayGenOIDN()
{
	uint2 LaunchIndex = DispatchRaysIndex().xy;
	uint2 LaunchDimensions = DispatchRaysDimensions().xy;

	float2 jt = 0.5;	//Hash3(uint3(LaunchIndex, iSample)).xy;	//jitter…は、掛けると輝点がジリジリしがちなのでやめた方がいい
	float2 uv = (LaunchIndex + jt) / Resolution;
	float2 d =  uv*2-1;

	// Setup the ray
	RayDesc ray;
	float3x3 camMat = {CameraRight, CameraUp, CameraForward};
	if (Perspective) {
		ray.Origin = CameraPosition;
		ray.Direction = mul(normalize(float3(d.x*AspectRatio,-d.y,CameraFoV)),camMat);
	} else {
		ray.Origin = CameraPosition - CameraUp * d.y / ProjectionMatrix._22 + CameraRight * d.x / ProjectionMatrix._11;
		ray.Direction = CameraForward;
	}
	ray.TMin = 1e-3;
	ray.TMax = 1e+5;

	// Trace the ray
	PayloadOIDN payload;
	payload.ID = -1;
	payload.iFace = -1;
	payload.Albedo = 0;
	payload.Normal = 0;
	payload.Tr = 1;
	payload.t = 0;
	payload.st = 0;

	TraceRay(TLAS,RAY_FLAG_NONE,0xFF,0,1,0,ray,payload);

	//OIDNへの入力
	uint idx = LaunchIndex.y * Resolution.x + LaunchIndex.x;
	OIDNBuf[idx].color = RTOutput[LaunchIndex].xyz;
	OIDNBuf[idx].albedo = saturate(payload.Albedo);
	OIDNBuf[idx].normal = clamp(payload.Normal,-1,1);

	//ポストプロセスへの入力
	uint2 p = LaunchIndex;
	NormalDepth[p] = float4(payload.Normal, payload.t * abs(dot(CameraForward,ray.Direction)));
	GBuffer1[p] = uint2(payload.ID, payload.iFace);
	GBuffer2[p] = payload.st;
}

[shader("miss")]
void MissOIDN(inout PayloadOIDN payload){
	payload.Normal = 0;
}

[shader("closesthit")]
void ClosestHitOIDN(inout PayloadOIDN payload, Attribute attrib){
	payload.ID = InstanceID();
	payload.t = RayTCurrent();
	payload.iFace = PrimitiveIndex();
	payload.st = attrib.st;
}

[shader("anyhit")]
void AnyHitOIDN(inout PayloadOIDN payload, Attribute attrib)
{
	uint id = InstanceID();
	Material m;
	float3x3 TBN;
	GetPatch(id, PrimitiveIndex(), 0, attrib.st, m, TBN);
	float3 N = TBN[2];
	float3 rd = normalize(WorldRayDirection());

	//片面ポリゴンの裏向き面はスルー
	float3 Ng = GeometryNormal(id, PrimitiveIndex());
	if ((dot(rd, Ng) > 0) && (m.twosided==0)) {
		IgnoreHit();
	}

	if (m.alpha < Dayo::AlphaThreshold) {
		IgnoreHit();
	}
	
	payload.Albedo = m.albedo;
	payload.Normal = N;

}
#endif
