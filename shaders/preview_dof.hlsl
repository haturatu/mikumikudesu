// Thin-lens, signed-CoC near/far DoF. Independent implementation; technique
// references and approximation limits are documented in render-effects.md.
struct DofConstants {
    uint width;
    uint height;
    uint mode;
    uint samples;
    // focus in metres, CoC scale in pixels, max radius in pixels, metres/unit
    float4 lens;
    // perspective focal (or ortho extent), aspect, perspective flag, SSS strength
    float4 projection;
};
[[vk::push_constant]] ConstantBuffer<DofConstants> dof;
[[vk::binding(0, 0)]] Texture2D<float4> colorTexture;
[[vk::binding(1, 0)]] SamplerState clampSampler;
[[vk::binding(2, 0)]] Texture2D<float> depthTexture;
[[vk::binding(3, 0)]] Texture2D<float4> skinTexture;
[[vk::binding(4, 0)]] Texture2D<float> cocTexture;
[[vk::binding(5, 0)]] Texture2D<float4> nearTexture;
[[vk::binding(6, 0)]] Texture2D<float4> farTexture;
[[vk::binding(7, 0)]] Texture2D<float4> halfData;
[[vk::binding(8, 0)]] Texture2D<float4> tileTexture;
[[vk::binding(9, 0)]] Texture2D<float4> nearBlur;
[[vk::binding(10, 0)]] Texture2D<float4> farBlur;
#ifdef DAYO_GLSLC
#define DAYO_RGBA [[spv::format_rgba16f]]
#else
#define DAYO_RGBA [[vk::image_format("rgba16f")]]
#endif
[[vk::binding(11, 0)]] DAYO_RGBA RWTexture2D<float4> outputColor;
[[vk::binding(12, 0)]] DAYO_RGBA RWTexture2D<float4> outputNear;
[[vk::binding(13, 0)]] DAYO_RGBA RWTexture2D<float4> outputFar;
[[vk::binding(14, 0)]]
#ifdef DAYO_GLSLC
#ifdef DAYO_DOF_R32
[[spv::format_r32f]]
#else
[[spv::format_r16f]]
#endif
#else
#ifdef DAYO_DOF_R32
[[vk::image_format("r32f")]]
#else
[[vk::image_format("r16f")]]
#endif
#endif
RWTexture2D<float> outputCoc;

float viewZ(float depth) {
    return dof.projection.z > 0.5 ? 5.0 / max(100.0 - depth * 99.95, 0.0001)
                                 : 0.05 + depth * 99.95;
}
int2 fullPixel(int2 pixel) { return clamp(pixel, int2(0,0), int2(dof.width, dof.height) - 1); }
int2 halfSize() { return int2((dof.width + 1) / 2, (dof.height + 1) / 2); }
int2 halfPixel(int2 pixel) { return clamp(pixel, int2(0,0), halfSize() - 1); }

float4 scatterSkin(int2 pixel) {
    const float4 original = colorTexture.Load(int3(pixel, 0));
    const float4 center = skinTexture.Load(int3(pixel, 0));
    if (center.a <= 0.01 || original.a <= 0.001 || dof.projection.w <= 0.0) return original;
    const float centerZ = viewZ(depthTexture.Load(int3(pixel, 0)));
    const float pixelsPerUnit = 0.5 * float(dof.height) *
        (dof.projection.z > 0.5 ? dof.projection.x / max(centerZ, 0.05) : 1.0 / max(dof.projection.x, 0.01));
    const float radius = clamp(pixelsPerUnit * 0.025 * dof.projection.w, 0.0, 24.0);
    if (radius < 0.5) return original;
    const float2 uv = (float2(pixel) + 0.5) / float2(dof.width, dof.height);
    float3 total = center.rgb;
    float3 weights = 1.0.xxx;
    [loop] for (uint tap = 0; tap < 16; ++tap) {
        const float fraction = (float(tap) + 0.5) / 16.0;
        const float angle = float(tap) * 2.39996322973;
        const float2 sampleUv = uv + float2(cos(angle), sin(angle)) * sqrt(fraction) * radius / float2(dof.width, dof.height);
        const float4 sample = skinTexture.SampleLevel(clampSampler, sampleUv, 0);
        const int2 q = fullPixel(int2(sampleUv * float2(dof.width, dof.height)));
        const float sampleZ = viewZ(depthTexture.Load(int3(q, 0)));
        const float edge = exp(-abs(sampleZ - centerZ) / 0.02) * saturate(sample.a);
        const float3 weight = exp(-fraction * float3(2.0, 5.0, 10.0)) * edge;
        total += sample.rgb * weight;
        weights += weight;
    }
    const float3 delta = (total / weights - center.rgb) * saturate(center.a * dof.projection.w);
    return float4(max(original.rgb + delta * original.a, 0.0), original.a);
}

[numthreads(8, 8, 1)]
void DofFilter(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (dof.mode == 0 || dof.mode == 1 || dof.mode == 5) {
        if (id.x >= dof.width || id.y >= dof.height) return;
    } else if (dof.mode == 3 || dof.mode == 6) {
        if (any(p >= (halfSize() + 7) / 8)) return;
    } else if (any(p >= halfSize())) return;

    if (dof.mode == 0) {
        outputColor[p] = scatterSkin(p);
    } else if (dof.mode == 1) {
        const float z = viewZ(depthTexture.Load(int3(p, 0))) * dof.lens.w;
        outputCoc[p] = clamp(dof.lens.y * (dof.lens.x / max(z, 0.001) - 1.0), -dof.lens.z, dof.lens.z);
    } else if (dof.mode == 2) {
        // Keep the nearest foreground and farthest background separately.
        // Depth-compatible weights avoid averaging across a silhouette.
        float nearCoc = 0.0, farCoc = 0.0, nearZ = 100.0, farZ = 0.05;
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 q = fullPixel(p * 2 + int2(i & 1, i >> 1));
            const float c = cocTexture.Load(int3(q, 0));
            const float z = viewZ(depthTexture.Load(int3(q, 0)));
            nearCoc = max(nearCoc, c); farCoc = min(farCoc, c);
            if (c > 0.0) nearZ = min(nearZ, z);
            if (c < 0.0) farZ = max(farZ, z);
        }
        float4 n = 0.0.xxxx, f = 0.0.xxxx;
        float farWeight = 0.0;
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 q = fullPixel(p * 2 + int2(i & 1, i >> 1));
            const float c = cocTexture.Load(int3(q, 0));
            const float z = viewZ(depthTexture.Load(int3(q, 0)));
            const float4 color = colorTexture.Load(int3(q, 0));
            const float nw = c > 0.0 ? saturate(1.0 - abs(z - nearZ) / max(0.02, nearZ * 0.03)) : 0.0;
            const float fw = c < 0.0 ? saturate(1.0 - abs(z - farZ) / max(0.02, farZ * 0.03)) : 0.0;
            n += color * nw * 0.25; f += color * fw; farWeight += fw;
        }
        outputNear[p] = n;
        outputFar[p] = f / max(farWeight, 0.0001);
        outputColor[p] = float4(nearCoc * 0.5, -farCoc * 0.5, nearZ, farZ);
    } else if (dof.mode == 3) {
        float2 ranges = 0.0.xx;
        [loop] for (int y = 0; y < 8; ++y)
            [loop] for (int x = 0; x < 8; ++x)
                ranges = max(ranges, halfData.Load(int3(halfPixel(p * 8 + int2(x,y)), 0)).xy);
        outputNear[p] = float4(ranges, 0.0, 0.0);
    } else if (dof.mode == 6) {
        // Dilate tile radii so a near disk can cover an in-focus/background tile.
        const int radius = int(ceil(dof.lens.z / 16.0));
        const int2 size = (halfSize() + 7) / 8;
        float2 ranges = 0.0.xx;
        [loop] for (int y = -radius; y <= radius; ++y)
            [loop] for (int x = -radius; x <= radius; ++x) {
                const int2 q = clamp(p + int2(x,y), int2(0,0), size - 1);
                ranges = max(ranges, tileTexture.Load(int3(q, 0)).xy);
            }
        outputNear[p] = float4(ranges, 0.0, 0.0);
    } else if (dof.mode == 4) {
        const float4 center = halfData.Load(int3(p,0));
        const float2 range = tileTexture.Load(int3(p / 8, 0)).xy;
        if (max(range.x, range.y) < 0.5) {
            outputNear[p] = 0.0.xxxx; outputFar[p] = 0.0.xxxx; return;
        }
        const float search = max(1.0, max(range.x, range.y));
        float4 n = 0.0.xxxx, f = 0.0.xxxx;
        float fwTotal = 0.0;
        [loop] for (uint i = 0; i < dof.samples; ++i) {
            const float fraction = (float(i) + 0.5) / float(dof.samples);
            const float angle = float(i) * 2.39996322973;
            const float2 offset = float2(cos(angle), sin(angle)) * sqrt(fraction) * search;
            const int2 q = halfPixel(p + int2(round(offset)));
            const float distance = length(float2(q - p));
            const float4 data = halfData.Load(int3(q,0));
            // Scatter-as-gather: the source disk, rather than the receiver,
            // decides whether foreground covers this pixel.
            if (range.x >= 0.5) {
                const float nw = saturate(data.x - distance + 0.5) * smoothstep(0.25, 0.75, data.x) *
                                 search * search / max(data.x * data.x, 0.25) / float(dof.samples);
                n += nearTexture.Load(int3(q,0)) * nw;
            }
            if (range.y >= 0.5) {
                const float farRadius = center.x > 0.0 ? data.y : min(data.y, max(center.y, 0.5));
                const float depthWeight = center.x > 0.0 ? 1.0 : saturate(1.0 - abs(data.w - center.w) / max(0.05, center.w * 0.05));
                const float fw = saturate(farRadius - distance + 0.5) * (data.y > 0.25 ? 1.0 : 0.0) * depthWeight;
                f += farTexture.Load(int3(q,0)) * fw; fwTotal += fw;
            }
        }
        // A deterministic centre sample stabilizes subpixel disks.
        if (center.y > 0.25) { f += farTexture.Load(int3(p,0)); fwTotal += 1.0; }
        outputNear[p] = n / max(1.0, n.a);
        outputFar[p] = f / max(fwTotal, 0.0001);
    } else if (dof.mode == 5) {
        const float2 uv = (float2(p) + 0.5) / float2(dof.width, dof.height);
        const float coc = cocTexture.Load(int3(p,0));
        const float z = viewZ(depthTexture.Load(int3(p,0)));
        const float4 sharp = colorTexture.Load(int3(p,0));
        // Bilateral far upsampling preserves focused edges. Near coverage must
        // cross depth discontinuities, so it is deliberately reconstructed separately.
        const float2 hp = uv * float2(halfSize()) - 0.5;
        const int2 base = int2(floor(hp));
        const float2 fracPart = frac(hp);
        float4 farColor = 0.0.xxxx;
        float farWeight = 0.0;
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 corner = int2(i & 1, i >> 1);
            const int2 q = halfPixel(base + corner);
            const float4 data = halfData.Load(int3(q,0));
            const float2 w = lerp(1.0 - fracPart, fracPart, float2(corner));
            const float weight = w.x * w.y * (coc > 0.0 ? 1.0 : saturate(1.0 - abs(data.w - z) / max(0.02, z * 0.03)));
            farColor += farBlur.Load(int3(q,0)) * weight; farWeight += weight;
        }
        farColor /= max(farWeight, 0.0001);
        const float nearRadius = tileTexture.Load(int3(p / 16, 0)).x;
        const float4 nearColor = nearRadius >= 0.5 ? nearBlur.SampleLevel(clampSampler, uv, 0) : 0.0.xxxx;
        float4 result = lerp(sharp, farColor, smoothstep(0.5, 2.0, -coc));
        if (coc > 0.0) result = lerp(result, farColor, smoothstep(0.5, 2.0, coc));
        outputColor[p] = nearColor + result * (1.0 - saturate(nearColor.a));
    }
}
