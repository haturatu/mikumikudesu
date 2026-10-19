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

// Interpolate premultiplied layer color and its alpha-weighted metadata at
// exactly the same subpixel position. Empty half-resolution texels contribute
// zero weight, so their placeholder depth cannot pollute an edge sample.
struct LayerSample {
    float4 nearColor;
    float4 farColor;
    float4 data;
};
LayerSample layerAt(float2 position) {
    const float2 uv = position / float2(halfSize());
    LayerSample sample;
    sample.nearColor = nearTexture.SampleLevel(clampSampler, uv, 0);
    sample.farColor = farTexture.SampleLevel(clampSampler, uv, 0);
    sample.data = halfData.SampleLevel(clampSampler, uv, 0);
    sample.data.xz /= max(sample.nearColor.a, 0.0001);
    sample.data.yw /= max(sample.farColor.a, 0.0001);
    return sample;
}

float diskCoverage(float radius, float distance) {
    // Integrate an approximately one-pixel footprint across the source disk.
    return smoothstep(distance - 0.75, distance + 0.75, radius);
}

float4 smallCocColor(int2 pixel, float coc, float z) {
    const float radius = min(abs(coc), 2.0);
    const float2 uv = (float2(pixel) + 0.5) / float2(dof.width, dof.height);
    float4 total = colorTexture.Load(int3(pixel, 0));
    float totalWeight = 1.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            const float2 offset = float2(x,y) * radius * 0.6;
            const float2 sampleUv = uv + offset / float2(dof.width, dof.height);
            const int2 q = fullPixel(int2(sampleUv * float2(dof.width, dof.height)));
            const float sampleZ = viewZ(depthTexture.Load(int3(q,0)));
            const float sampleCoc = cocTexture.Load(int3(q,0));
            // A focused foreground must not enter a defocused far receiver.
            const float edge = saturate(1.0 - abs(sampleZ - z) / max(0.02, z * 0.03));
            const float side = coc * sampleCoc >= 0.0 ? 1.0 : 0.0;
            const float weight = diskCoverage(radius, length(offset)) * edge * side;
            total += colorTexture.SampleLevel(clampSampler, sampleUv, 0) * weight;
            totalWeight += weight;
        }
    }
    return total / max(totalWeight, 0.0001);
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
            if (c <= 0.0) farZ = max(farZ, z);
        }
        float4 n = 0.0.xxxx, f = 0.0.xxxx;
        float farWeight = 0.0;
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 q = fullPixel(p * 2 + int2(i & 1, i >> 1));
            const float c = cocTexture.Load(int3(q, 0));
            const float z = viewZ(depthTexture.Load(int3(q, 0)));
            const float4 color = colorTexture.Load(int3(q, 0));
            const float nw = c > 0.0 ? saturate(1.0 - abs(z - nearZ) / max(0.02, nearZ * 0.03)) : 0.0;
            const float fw = c <= 0.0 ? saturate(1.0 - abs(z - farZ) / max(0.02, farZ * 0.03)) : 0.0;
            n += color * nw * 0.25; f += color * fw; farWeight += fw;
        }
        outputNear[p] = n;
        const float4 farColor = f / max(farWeight, 0.0001);
        outputFar[p] = farColor;
        outputColor[p] = float4(nearCoc * 0.5 * n.a, -farCoc * 0.5 * farColor.a,
                                nearZ * n.a, farZ * farColor.a);
    } else if (dof.mode == 3) {
        float2 ranges = 0.0.xx;
        [loop] for (int y = 0; y < 8; ++y)
            [loop] for (int x = 0; x < 8; ++x)
                ranges = max(ranges, layerAt(float2(halfPixel(p * 8 + int2(x,y))) + 0.5).data.xy);
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
        const LayerSample center = layerAt(float2(p) + 0.5);
        const float2 range = tileTexture.Load(int3(p / 8, 0)).xy;
        if (max(range.x, range.y) < 0.25) {
            outputNear[p] = 0.0.xxxx; outputFar[p] = 0.0.xxxx; return;
        }
        float4 n = 0.0.xxxx, f = 0.0.xxxx;
        float nwTotal = 0.0, fwTotal = 0.0;
        const float nearSearch = max(0.5, range.x);
        const float farSearch = max(0.5, center.data.x > 0.0 ? max(range.x, range.y) : center.data.y);
        [loop] for (uint i = 0; i < dof.samples; ++i) {
            const float fraction = (float(i) + 0.5) / float(dof.samples);
            const float angle = float(i) * 2.39996322973;
            const float2 disk = float2(cos(angle), sin(angle)) * sqrt(fraction);
            if (range.x >= 0.25) {
                const float2 offset = disk * nearSearch;
                const LayerSample sample = layerAt(float2(p) + 0.5 + offset);
                const float distance = length(offset);
                const float weight = diskCoverage(sample.data.x, distance) * smoothstep(0.1, 0.6, sample.data.x) *
                                     nearSearch * nearSearch / max(sample.data.x * sample.data.x, 0.25);
                n += sample.nearColor * weight;
                // Normalize the finite sample pattern against its own disk
                // integral: constant foreground has unit coverage, independent
                // of radius and sample count. Never normalize by foreground
                // coverage, which would turn silhouette pixels opaque.
                nwTotal += diskCoverage(nearSearch, distance);
            }
            if ((range.y >= 0.25 || center.data.x > 0.0) && farSearch >= 0.25) {
                const float2 offset = disk * farSearch;
                const LayerSample sample = layerAt(float2(p) + 0.5 + offset);
                const float radius = center.data.x > 0.0 ? farSearch : min(sample.data.y, max(center.data.y, 0.25));
                const float depthWeight = center.data.x > 0.0 ? 1.0 :
                    saturate(1.0 - abs(sample.data.w - center.data.w) / max(0.05, center.data.w * 0.05));
                const float weight = diskCoverage(radius, length(offset)) *
                    ((center.data.x > 0.0 || sample.data.y > 0.0) && sample.farColor.a > 0.0 ? 1.0 : 0.0) * depthWeight;
                f += sample.farColor * weight; fwTotal += weight;
            }
        }
        outputNear[p] = n / max(nwTotal, 0.0001);
        float4 farResult = fwTotal > 0.0001 ? f / fwTotal : center.farColor;
        // Missing near-layer background is an occupancy hole, not transparent
        // geometry. Reconstruct the available opaque far radiance for that hole.
        if (center.data.x > 0.0 && farResult.a > 0.0001)
            farResult = float4(farResult.rgb / farResult.a, 1.0);
        outputFar[p] = farResult;
    } else if (dof.mode == 5) {
        const float2 uv = (float2(p) + 0.5) / float2(dof.width, dof.height);
        const float coc = cocTexture.Load(int3(p,0));
        const float z = viewZ(depthTexture.Load(int3(p,0)));
        const float4 original = colorTexture.Load(int3(p,0));
        const float4 sharp = abs(coc) > 0.5 && abs(coc) < 3.0 ?
            lerp(original, smallCocColor(p, coc, z), smoothstep(0.5, 1.5, abs(coc))) : original;
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
            const float4 data = layerAt(float2(q) + 0.5).data;
            const float2 w = lerp(1.0 - fracPart, fracPart, float2(corner));
            const float weight = w.x * w.y * (coc > 0.0 ? 1.0 : saturate(1.0 - abs(data.w - z) / max(0.02, z * 0.03)));
            farColor += farBlur.Load(int3(q,0)) * weight; farWeight += weight;
        }
        farColor /= max(farWeight, 0.0001);
        // The source is opaque even when the far layer has an occupancy
        // hole beneath it. Preserve opacity after interpolation, and retain
        // the source radiance if no visible background can reconstruct it.
        if (original.a >= 0.98)
            farColor = farColor.a > 0.0001 ? float4(farColor.rgb / farColor.a, 1.0) : original;
        const float nearRadius = tileTexture.Load(int3(p / 16, 0)).x;
        float4 nearColor = nearRadius >= 0.25 ? nearBlur.SampleLevel(clampSampler, uv, 0) : 0.0.xxxx;
        nearColor /= max(1.0, nearColor.a);
        float4 result = lerp(sharp, farColor, smoothstep(1.5, 3.0, -coc));
        if (coc > 0.0) result = lerp(result, farColor, smoothstep(1.5, 3.0, coc));
        const float4 composite = nearColor + result * (1.0 - nearColor.a);
        outputColor[p] = max(abs(coc), nearRadius * 2.0) < 0.5 ? original : composite;
    }
}
