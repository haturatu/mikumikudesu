#include "preview_quality.hlsli"
[numthreads(8, 8, 1)]
void ScreenReflection(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy);
    float4 surface = qualitySurfaceAt(uv);
    uint packed = uint(round(surface.a));
    float reflectivity = float(packed & 15U) / 15;
    float4 result = 0;
    float depth = qualityDepth.Load(int3(qualityPixel(uv), 0));
    if (reflectivity > 0 && surface.b < 0.7 && depth < 0.99999) {
        float3 p = qualityPosition(uv, qualityViewZ(depth));
        float3 n = decodeOctNormal(surface.rg);
        float3 direction = reflect(normalize(p), n);
        float distance = quality.post.z;
        float3 origin = p + n * max(0.004, p.z * 0.001);
        uint steps = uint(clamp(quality.temporal.w, 8, 48));
        float last = 0;
        [loop] for (uint i = 1; i <= steps; ++i) {
            float t = distance * float(i) / float(steps);
            float3 q = origin + direction * t;
            float2 qUv = qualityProject(q);
            if (q.z <= quality.projection.z || !qualityInside(qUv)) break;
            float delta = q.z - qualityDepthAt(qUv);
            if (delta > 0 && delta < 0.025 + t * 0.025 && length((qUv - uv) * quality.size) > 2) {
                // The hit must face the ray. Reject the back of the originating
                // normal-mapped surface before accepting a short-distance hit.
                float3 hitNormal = decodeOctNormal(qualityNormal.Load(int3(qualityPixel(qUv), 0)));
                if (dot(hitNormal, direction) >= -0.02) { last = t; continue; }
                // Refine the intersection before sampling scene colour.
                float low = last, high = t;
                [unroll] for (uint j = 0; j < 3; ++j) {
                    float mid = (low + high) * 0.5;
                    float3 hit = origin + direction * mid;
                    if (hit.z > qualityDepthAt(qualityProject(hit))) high = mid; else low = mid;
                }
                qUv = qualityProject(origin + direction * high);
                float4 hitColor = qualityColor.SampleLevel(qualityLinearSampler, qUv, 0);
                float edge = saturate(min(min(qUv.x, qUv.y), min(1 - qUv.x, 1 - qUv.y)) * 12);
                result = float4(hitColor.rgb / max(hitColor.a, 1e-5),
                                reflectivity * edge * (1 - surface.b * surface.b) * hitColor.a);
                break;
            }
            last = t;
        }
    }
    qualityOutput[id.xy] = result;
}
[numthreads(8, 8, 1)]
void ScreenComposite(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= quality.outputSize)) return;
    float2 uv = qualityUv(id.xy);
    float4 color = qualityColor.Load(int3(id.xy, 0));
    float4 surface = qualitySurfaceAt(uv);
    uint packed = uint(round(surface.a));
    float transmission = float((packed >> 4) & 15U) / 15;
    float z = qualityDepthAt(uv);
    float3 n = decodeOctNormal(surface.rg);
    if (quality.effects.x > 0) {
        float4 reflection = 0;
        float weights = 0;
        // Depth/normal bilateral upsampling keeps half-resolution reflections
        // from leaking across silhouettes and adjacent materials.
        [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
            float2 q = uv + (float2(x, y) - 0.5) * 2 / quality.size;
            float4 other = qualitySurfaceAt(q);
            float w = exp(-abs(qualityDepthAt(q) - z) / max(0.01, z * 0.01)) *
                      pow(saturate(dot(n, decodeOctNormal(other.rg))), 8);
            reflection += qualityAuxiliary.SampleLevel(qualityLinearSampler, q, 0) * w;
            weights += w;
        }
        reflection /= max(weights, 1e-5);
        color.rgb = lerp(color.rgb, reflection.rgb * color.a, saturate(reflection.a * quality.effects.x));
    }
    if (quality.effects.y > 0 && transmission > 0 && color.a > 0) {
        float eta = 1 / max(quality.effects.z, 1);
        float2 shift = n.xy * quality.effects.w * (1 - eta) * quality.projection.x / max(z, 0.1);
        float2 q = uv + shift * float2(1 / quality.projection.y, -1);
        if (qualityInside(q) && qualityDepthAt(q) >= z - max(0.01, z * 0.005)) {
            float4 refracted = qualityColor.SampleLevel(qualityLinearSampler, q, 0);
            // This thin-surface approximation is limited to Glass materials;
            // keep original coverage and fall back when the background is absent.
            float amount = saturate(transmission * quality.effects.y) * step(0.01, refracted.a);
            color.rgb = lerp(color.rgb, refracted.rgb / max(refracted.a, 1e-5) * color.a, amount);
        }
    }
    qualityOutput[id.xy] = color;
}
