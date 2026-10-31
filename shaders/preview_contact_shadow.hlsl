#include "preview_quality.hlsli"
[numthreads(8, 8, 1)]
void ContactShadow(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy);
    float depth = qualityDepth.Load(int3(qualityPixel(uv), 0));
    float visibility = 1;
    if (depth < 0.99999) {
        float3 p = qualityPosition(uv, qualityViewZ(depth));
        float3 n = decodeOctNormal(qualityNormal.Load(int3(qualityPixel(uv), 0)));
        float3 l = normalize(quality.light.xyz);
        float rayLength = quality.light.w;
        float bias = max(0.0015, p.z * 0.0005);
        uint steps = uint(clamp(quality.temporal.w, 4, 12));
        if (dot(n, l) > 0.03) {
            [loop] for (uint i = 1; i <= steps; ++i) {
                float distance = rayLength * float(i) / float(steps);
                float3 q = p + n * bias * 2 + l * distance;
                float2 qUv = qualityProject(q);
                if (!qualityInside(qUv) || q.z <= quality.projection.z) break;
                if (length((qUv - uv) * quality.size) < 1.0) continue;
                float delta = q.z - qualityDepthAt(qUv);
                // A bounded thickness rejects deep silhouettes and the biased
                // ray origin prevents a receiver from shadowing itself.
                if (delta > bias && delta < max(0.015, rayLength * 0.2)) {
                    visibility = 1 - saturate(1.0 - distance / max(rayLength * 2, 1e-5));
                    break;
                }
            }
        }
    }
    qualityOutput[id.xy] = visibility.xxxx;
}
[numthreads(8, 8, 1)]
void ContactBlur(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy);
    float z = qualityDepthAt(uv), sum = 0, weight = 0;
    [unroll] for (int i = -2; i <= 2; ++i) {
        float2 q = uv + float2(i & 1 ? i : 0, i & 1 ? 0 : i * 0.5) / quality.outputSize;
        float w = exp(-abs(qualityDepthAt(q) - z) / max(0.005, z * 0.005));
        sum += qualityAuxiliary.SampleLevel(qualityPointSampler, q, 0).r * w;
        weight += w;
    }
    qualityOutput[id.xy] = (sum / max(weight, 1e-5)).xxxx;
}
