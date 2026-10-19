// Reference/adaptation notices: docs/preview/THIRD_PARTY_NOTICES.md
// Colour management shared by the preview tone mapping pass and the in-shader
// fallback used when the scene is composited on top of a previous frame.
//
// Values are linear scene-referred RGB and the output is display-referred
// linear RGB; the sRGB swapchain performs the transfer encode in hardware.

// Match the view transform ids used by graphics::PreviewViewTransform.
#define DAYO_VIEW_ACES 0
#define DAYO_VIEW_AGX 1
#define DAYO_VIEW_NEUTRAL 2

float3 acesFilm(float3 value) {
    return saturate((value * (2.51 * value + 0.03)) / max(value * (2.43 * value + 0.59) + 0.14, 1e-5));
}

// AgX in its compact form: transform into the AgX working space, encode with a
// fixed log range, apply the sigmoid fit and rotate back. This is what keeps
// saturated character highlights from clipping to flat white.
//
// The rotation rows are locals rather than nested constructors because the
// glslc HLSL front end mis-parses `float3(dot(a, float3(...)), ...)`.
float3 agxInTransform(float3 c) {
    const float3 rowX = float3(0.842479062253094, 0.0423282422610123, 0.0423756549057051);
    const float3 rowY = float3(0.0784335999999992, 0.878468636469772, 0.0784336);
    const float3 rowZ = float3(0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    return float3(dot(c, rowX), dot(c, rowY), dot(c, rowZ));
}

float3 agxOutTransform(float3 c) {
    const float3 rowX = float3(1.19687900512017, -0.0980208811401368, -0.0990297440797205);
    const float3 rowY = float3(-0.0528968517574562, 1.15190312990417, -0.0989611768448433);
    const float3 rowZ = float3(-0.0529716355144438, -0.0980434501171241, 1.15107367264116);
    return float3(dot(c, rowX), dot(c, rowY), dot(c, rowZ));
}

float3 agxContrast(float3 x) {
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232 +
           31.96 * x4;
}

float3 agx(float3 color) {
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    color = agxInTransform(max(color, 0.0));
    color = clamp(log2(max(color, 1e-10)), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    // AgX produces a display-encoded signal; linearize before hardware sRGB encoding.
    return pow(max(agxOutTransform(agxContrast(color)), 0.0), 2.2);
}

// Khronos PBR Neutral: preserves in-gamut colours and only compresses the
// highlight range, so flat colours stay exactly as authored.
float3 neutralTonemap(float3 x) {
    const float startCompression = 0.76;
    const float desaturation = 0.15;
    float xMin = min(min(x.r, x.g), x.b);
    float offset = xMin < 0.08 ? xMin - 6.25 * xMin * xMin : 0.04;
    x -= offset;
    float peak = max(max(x.r, x.g), x.b);
    if (peak < startCompression)
        return x;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    x *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(x, newPeak.xxx, g);
}

float3 applyViewTransform(float3 color, uint transform) {
    if (transform == DAYO_VIEW_AGX)
        return agx(color);
    if (transform == DAYO_VIEW_NEUTRAL)
        return saturate(neutralTonemap(color));
    return acesFilm(color);
}

// Planckian locus chromaticity (Kim et al.) compared against D65, normalised to
// unit luminance so the tint never brightens or darkens the image.
float3 planckianRgb(float kelvin) {
    const float t = clamp(kelvin, 1000.0, 15000.0);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float x = t < 4000.0 ? (-0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910)
                               : (-3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390);
    const float y = t < 2222.0 ? (-1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683)
                   : t < 4000.0
                       ? (-0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867)
                       : (3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483);
    const float3 xyz = float3(x / max(y, 1e-4), 1.0, (1.0 - x - y) / max(y, 1e-4));
    const float3 redRow = float3(3.2406, -1.5372, -0.4986);
    const float3 greenRow = float3(-0.9689, 1.8758, 0.0415);
    const float3 blueRow = float3(0.0557, -0.2040, 1.0570);
    return max(float3(dot(redRow, xyz), dot(greenRow, xyz), dot(blueRow, xyz)), 0.05);
}

float3 whiteBalanceGain(float kelvin) {
    // Anchor at the default white point so legacy ACES projects retain their tint.
    const float3 gain = planckianRgb(6500.0) / planckianRgb(kelvin);
    return gain / max(dot(gain, float3(0.2126, 0.7152, 0.0722)), 1e-4);
}

float3 applyExposureAndWhiteBalance(float3 color, float exposureEv, float kelvin) {
    return color * exp2(exposureEv) * whiteBalanceGain(kelvin);
}
