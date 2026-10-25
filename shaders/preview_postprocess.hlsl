#include "preview_quality.hlsli"
#include "preview_color.hlsli"
float luminance(float3 rgb) { return dot(rgb, float3(0.2126, 0.7152, 0.0722)); }
float3 toYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), (c.r - c.b) * 0.5, c.g * 0.5 - (c.r + c.b) * 0.25); }
float3 fromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
[numthreads(8, 8, 1)]
void TemporalResolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    int2 pixel = int2(id.xy);
    float2 uv = qualityUv(id.xy);
    float4 current = qualityColor.Load(int3(pixel, 0));
    float4 motion = qualityMotion.Load(int3(pixel, 0));
    float4 surface = qualitySurface.Load(int3(pixel, 0));
    float2 previousUv = uv + motion.xy;
    float weight = 0;
    float4 result = current;
    if (quality.temporal.y > 0.5 && motion.w > 0.5 && qualityInside(previousUv)) {
        float4 geometry = qualityHistoryGeometry.SampleLevel(qualityPointSampler, previousUv, 0);
        float3 normal = decodeOctNormal(qualityNormal.Load(int3(pixel, 0)));
        float3 oldNormal = decodeOctNormal(geometry.gb);
        bool visible = abs(geometry.r - motion.z) <= max(0.015, motion.z * 0.025) &&
                       dot(normal, oldNormal) > 0.7 && abs(current.a - geometry.a) < 0.2;
        if (visible) {
            float4 previous = qualityHistory.SampleLevel(qualityLinearSampler, previousUv, 0);
            float3 low = 1e20.xxx, high = -1e20.xxx;
            float alphaLow = current.a, alphaHigh = current.a;
            [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
                int2 q = clamp(pixel + int2(x, y), 0, int2(quality.size) - 1);
                float4 neighbor = qualityColor.Load(int3(q, 0));
                float3 c = toYCoCg(neighbor.rgb);
                low = min(low, c); high = max(high, c);
                alphaLow = min(alphaLow, neighbor.a); alphaHigh = max(alphaHigh, neighbor.a);
            }
            previous.rgb = fromYCoCg(clamp(toYCoCg(previous.rgb), low, high));
            previous.a = clamp(previous.a, alphaLow, alphaHigh);
            float reactive = max(qualityReactive(surface), saturate(abs(current.a - geometry.a) * 5));
            float response = saturate(abs(luminance(previous.rgb) - luminance(current.rgb)) /
                                      max(0.05, max(luminance(previous.rgb), luminance(current.rgb))));
            weight = quality.temporal.x * (1 - reactive) * (1 - response * 0.6);
            result = lerp(current, previous, weight);
        }
    }
    // Blended hair and glass retain current-frame coverage, rather than leaving
    // alpha trails on a transparent background.
    result.a = current.a;
    qualityOutput[id.xy] = max(result, 0);
    qualityGeometryOutput[id.xy] = float4(qualityDepthAt(uv), qualityNormal.Load(int3(pixel, 0)), current.a);
}
[numthreads(8, 8, 1)]
void PrepareDisplay(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy);
    float4 sample = qualityColor.Load(int3(id.xy, 0));
    float alpha = saturate(sample.a);
    float3 bloom = qualityAuxiliary.SampleLevel(qualityLinearSampler, uv, 0).rgb * quality.post.y;
    float3 color = sample.rgb / max(alpha, 1e-5) + bloom;
    color = applyViewTransform(color * quality.color.w * quality.color.xyz, uint(quality.post.x)) * alpha;
    if (alpha < 1)
        color += applyViewTransform(bloom * quality.color.w * quality.color.xyz, uint(quality.post.x)) * (1 - alpha);
    qualityOutput[id.xy] = float4(saturate(color), alpha);
}
float4 displayAt(float2 uv) { return qualityColor.SampleLevel(qualityLinearSampler, uv, 0); }
[numthreads(8, 8, 1)]
void FxaaResolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy), texel = 1.0 / quality.size;
    float4 center = displayAt(uv);
    float4 nw = displayAt(uv + float2(-1,-1) * texel), ne = displayAt(uv + float2(1,-1) * texel);
    float4 sw = displayAt(uv + float2(-1,1) * texel), se = displayAt(uv + float2(1,1) * texel);
    float a = luminance(nw.rgb), b = luminance(ne.rgb), c = luminance(sw.rgb), d = luminance(se.rgb);
    float low = min(luminance(center.rgb), min(min(a,b), min(c,d)));
    float high = max(luminance(center.rgb), max(max(a,b), max(c,d)));
    float4 result = center;
    if (high - low > max(0.0312, high * 0.125)) {
        float2 direction = float2(-((a+b)-(c+d)), (a+c)-(b+d));
        float reduce = max((a+b+c+d) * 0.03125, 0.0078125);
        direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduce), -8, 8) * texel;
        float4 first = 0.5 * (displayAt(uv + direction * (-1.0/6.0)) + displayAt(uv + direction * (1.0/6.0)));
        float4 second = first * 0.5 + 0.25 * (displayAt(uv - direction * 0.5) + displayAt(uv + direction * 0.5));
        float l = luminance(second.rgb);
        result = l < low || l > high ? first : second;
    }
    qualityOutput[id.xy] = result;
}
