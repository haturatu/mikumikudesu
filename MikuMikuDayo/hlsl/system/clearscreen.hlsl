struct VSO {
	float4 pos:SV_POSITION;
	float2 uv:TEXCOORD;
};

VSO VS( float4 pos : POSITION, float2 uv:TEXCOORD )
{
	VSO o = (VSO)0;
	o.pos = pos;
	o.uv = uv;
	return o;
}

float4 PS(VSO vso) : SV_TARGET
{
	return float4(1,0.5,0.2,1);
}

