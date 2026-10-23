#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dayo::graphics {
namespace detail {
// Same Planckian-locus fit as preview_color.hlsli, evaluated once when render
// settings change rather than for every FXAA neighbour and bloom sample.
inline std::array<float, 3> previewPlanckianRgb(float kelvin) noexcept {
    const float t = std::clamp(kelvin, 1000.0F, 15000.0F);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float x = t < 4000.0F ? (-0.2661239e9F / t3 - 0.2343589e6F / t2 + 0.8776956e3F / t + 0.179910F)
                                : (-3.0258469e9F / t3 + 2.1070379e6F / t2 + 0.2226347e3F / t + 0.240390F);
    const float y = t < 2222.0F   ? (-1.1063814F * x * x * x - 1.34811020F * x * x + 2.18555832F * x - 0.20219683F)
                    : t < 4000.0F ? (-0.9549476F * x * x * x - 1.37418593F * x * x + 2.09137015F * x - 0.16748867F)
                                  : (3.0817580F * x * x * x - 5.87338670F * x * x + 3.75112997F * x - 0.37001483F);
    const float denominator = std::max(y, 1e-4F);
    const float X = x / denominator;
    const float Z = (1.0F - x - y) / denominator;
    return {
        std::max(3.2406F * X - 1.5372F - 0.4986F * Z, 0.05F),
        std::max(-0.9689F * X + 1.8758F + 0.0415F * Z, 0.05F),
        std::max(0.0557F * X - 0.2040F + 1.0570F * Z, 0.05F),
    };
}
} // namespace detail

// xyz = white-balance gain, w = exposure multiplier. Keep these separate so
// shader evaluation retains the original (color * exposure) * white-balance order.
// Callers supply sanitized PreviewRenderSettings values.
inline std::array<float, 4> previewColorAdjustment(float exposureEv, float kelvin) noexcept {
    const auto reference = detail::previewPlanckianRgb(6500.0F);
    const auto whitePoint = detail::previewPlanckianRgb(kelvin);
    const std::array gain{reference[0] / whitePoint[0], reference[1] / whitePoint[1], reference[2] / whitePoint[2]};
    const float luminance = std::max(gain[0] * 0.2126F + gain[1] * 0.7152F + gain[2] * 0.0722F, 1e-4F);
    return {gain[0] / luminance, gain[1] / luminance, gain[2] / luminance, std::exp2(exposureEv)};
}
} // namespace dayo::graphics
