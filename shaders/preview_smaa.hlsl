// SMAA 1x port using the original area/search tables and pattern searches.
// iryoku/smaa, revision 71c806a838bdd7d517df19192a20f0c61b3ca29d.
// Copyright and MIT license: ../third_party/smaa/LICENSE.txt.
#include "preview_quality.hlsli"
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS float4(1.0 / float2(quality.size), float2(quality.size))
#define SMAATexture2D(tex) Texture2D<float4> tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(qualityLinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(qualityPointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(qualityLinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(qualityLinearSampler, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(qualityPointSampler, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(qualityLinearSampler, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#include "../third_party/smaa/SMAA.hlsl"
float edgeSignal(float4 c) { return dot(pow(saturate(c.rgb), 1.0 / 2.2), float3(0.2126, 0.7152, 0.0722)); }
[numthreads(8, 8, 1)]
void SmaaEdges(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy);
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
    float4 center = qualityColor.Load(int3(id.xy, 0));
    float4 left = SMAASamplePoint(qualityColor, offset[0].xy), top = SMAASamplePoint(qualityColor, offset[0].zw);
    float l = edgeSignal(center);
    float2 delta = max(abs(l - float2(edgeSignal(left), edgeSignal(top))), abs(center.a - float2(left.a, top.a)));
    float2 edges = step(SMAA_THRESHOLD.xx, delta);
    float4 right = SMAASamplePoint(qualityColor, offset[1].xy), bottom = SMAASamplePoint(qualityColor, offset[1].zw);
    float2 other = max(abs(l - float2(edgeSignal(right), edgeSignal(bottom))), abs(center.a - float2(right.a, bottom.a)));
    float2 distant = abs(float2(edgeSignal(left), edgeSignal(top)) -
                         float2(edgeSignal(SMAASamplePoint(qualityColor, offset[2].xy)), edgeSignal(SMAASamplePoint(qualityColor, offset[2].zw))));
    float maximum = max(max(max(delta.x, delta.y), max(other.x, other.y)), max(distant.x, distant.y));
    edges *= step(maximum, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta);
    qualityOutput[id.xy] = float4(edges, 0, 0);
}
[numthreads(8, 8, 1)]
void SmaaWeights(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy), pixel;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, pixel, offset);
    qualityOutput[id.xy] = SMAABlendingWeightCalculationPS(uv, pixel, offset, qualityColor, qualityArea, qualitySearch, 0.0.xxxx);
}
[numthreads(8, 8, 1)]
void SmaaNeighborhood(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy);
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    qualityOutput[id.xy] = SMAANeighborhoodBlendingPS(uv, offset, qualityColor, qualityAuxiliary);
}
