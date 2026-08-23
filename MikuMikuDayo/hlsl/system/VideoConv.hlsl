
ByteAddressBuffer src		: register(t0); //変換元のフレーム
RWTexture2D<float4> dest 	: register(u0); //出力テクスチャ

float3 YUV2RGB(uint3 yuv)
{
    float3 YUV = yuv/255.0 - float3(0,0.5,0.5);
    float3x3 m = {
        1,0,1.402,
        1,-0.344136,-0.714136,
        1,1.772,0
    };

    return saturate(mul(m,YUV));
}

uint3 GetDimPitch()
{
    uint w,h,pitch;
    dest.GetDimensions(w,h);
    pitch = (w/2+7) &(~7);
    return uint3(w,h,pitch);
}

static uint W = GetDimPitch().x;
static uint H = GetDimPitch().y;
static uint RowPitch = GetDimPitch().z;

//YUV422, YUY2 → RGBA x 2pixels
[numthreads(8, 8, 1)]
void SystemVideoCSYUY2(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // YUY2バッファからデータを読み込む
    uint2 p = dispatchThreadID.xy;
    uint index = (p.y * RowPitch + p.x) * 4;

    //読み書きについて範囲外アクセスのチェックしてないけどいいのかわからん
    uint s = src.Load(index);
    uint y0 = s & 0xFF;
    uint u = s >> 8 & 0xFF;
    uint y1 = s >> 16 & 0xFF;
    uint v = s >> 24;

    float3 rgb0 = YUV2RGB(uint3(y0,u,v));
    float3 rgb1 = YUV2RGB(uint3(y1,u,v));

    dest[uint2(p.x*2, p.y)] = float4(rgb0, 1.0f);
    dest[uint2(p.x*2+1, p.y)] = float4(rgb1, 1.0f);
}

//YUV420, NV12 → RGBA
[numthreads(8, 8, 1)]
void SystemVideoCSNV12(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadID.xy;
    uint index = (p.y * RowPitch + p.x) * 4;

    //ハマった話…Loadに指定するインデックスは4バイトアラインしないとダメ
    //アラインしなくてもちゃんと読み込めてしまう環境も有る
    uint yyyy = src.Load(index);
    uint y[4];
    y[0] = yyyy & 0xFF;
    y[1] = yyyy >> 8 & 0xFF;
    y[2] = yyyy >> 16 & 0xFF;
    y[3] = yyyy >> 24;

    uint o = H*RowPitch;   //yが占める領域
    uint uv = src.Load(o + (p.y/2*RowPitch + p.x*4));
    uint u[2],v[2];
    u[0] = uv & 0xFF;
    v[0] = uv >> 8 & 0xFF;
    u[1] = uv >> 16 & 0xFF;
    v[1] = uv >> 24;

    for (int i=0; i<4; i++) 
        dest[uint2(p.x*4+i, p.y)] = float4(YUV2RGB(uint3(y[i],u[i/2],v[i/2])), 1.0f);
}

//ARGB → RGBA
[numthreads(8, 8, 1)]
void SystemVideoCSARGB(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 p = dispatchThreadID.xy;

    uint s = src.Load((p.y*RowPitch + p.x)*4);
    uint a = s & 0xFF;
    uint r = s >> 8 & 0xFF;
    uint g = s >> 16 & 0xFF;
    uint b = s >> 24;
    dest[uint2(p.x, p.y)] = float4(r,g,b,a)/255.0;
}
