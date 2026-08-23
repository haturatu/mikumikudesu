#include "../yrz.hlsli"
#include "../resources_deform.hlsli"

#include "../skinning.hlsli"

using namespace Dayo;

[numthreads(1024, 1, 1)]
void CS( uint3 id : SV_DispatchThreadID )
{
    uint idx = id.x;
    OutBuf[idx] = DefaultSkinning(idx);
}

