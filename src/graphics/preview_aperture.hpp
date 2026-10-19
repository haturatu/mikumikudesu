#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <vector>

namespace dayo::graphics {

inline constexpr std::uint32_t previewApertureSize = 128;
using PreviewApertureLut = std::array<float, previewApertureSize * previewApertureSize>;

// Cubic edges interpolate between a regular polygon and circular arcs.
// Rasterize radial intersections with four subpixel coverage samples. The
// resulting mask changes only when the aperture shape controls change.
inline std::array<float, 512> previewApertureRadii(std::uint32_t blades, float curvature, float rotationDegrees) {
    constexpr float tau = 2.0F * std::numbers::pi_v<float>;
    constexpr std::size_t angularSamples = 512;
    std::array<float, angularSamples> radii{};
    radii.fill(1.0F);
    if (blades >= 5 && blades <= 12) {
        using Point = std::array<float, 2>;
        std::vector<Point> boundary;
        const float step = tau / static_cast<float>(blades);
        const float rotation = rotationDegrees * std::numbers::pi_v<float> / 180.0F;
        const float k = 4.0F / 3.0F * std::tan(step * 0.25F);
        curvature = std::clamp(curvature, 0.0F, 1.0F);
        for (std::uint32_t blade = 0; blade < blades; ++blade) {
            const float a = rotation + static_cast<float>(blade) * step;
            const float b = a + step;
            const Point start{std::cos(a), std::sin(a)};
            const Point end{std::cos(b), std::sin(b)};
            Point control1{}, control2{};
            for (std::size_t axis = 0; axis < 2; ++axis) {
                const float tangent1 = axis == 0 ? -std::sin(a) : std::cos(a);
                const float tangent2 = axis == 0 ? -std::sin(b) : std::cos(b);
                control1[axis] =
                    std::lerp(std::lerp(start[axis], end[axis], 1.0F / 3.0F), start[axis] + k * tangent1, curvature);
                control2[axis] =
                    std::lerp(std::lerp(start[axis], end[axis], 2.0F / 3.0F), end[axis] - k * tangent2, curvature);
            }
            for (int sample = 0; sample < 16; ++sample) {
                const float t = static_cast<float>(sample) / 16.0F;
                const float u = 1.0F - t;
                Point point{};
                for (std::size_t axis = 0; axis < 2; ++axis)
                    point[axis] = u * u * u * start[axis] + 3.0F * u * u * t * control1[axis] +
                                  3.0F * u * t * t * control2[axis] + t * t * t * end[axis];
                boundary.push_back(point);
            }
        }
        const auto cross = [](const Point& a, const Point& b) { return a[0] * b[1] - a[1] * b[0]; };
        for (std::size_t angle = 0; angle < radii.size(); ++angle) {
            const float theta = static_cast<float>(angle) * tau / static_cast<float>(radii.size());
            const Point direction{std::cos(theta), std::sin(theta)};
            for (std::size_t edge = 0; edge < boundary.size(); ++edge) {
                const auto& a = boundary[edge];
                const auto& b = boundary[(edge + 1) % boundary.size()];
                const Point delta{b[0] - a[0], b[1] - a[1]};
                const float denominator = cross(direction, delta);
                if (std::abs(denominator) < 0.000001F)
                    continue;
                const float t = cross(a, delta) / denominator;
                const float u = cross(a, direction) / denominator;
                if (t > 0.0F && u >= 0.0F && u <= 1.0F)
                    radii[angle] = std::min(radii[angle], t);
            }
        }
    }
    return radii;
}

inline PreviewApertureLut previewApertureLut(std::uint32_t blades, float curvature, float rotationDegrees) {
    constexpr float tau = 2.0F * std::numbers::pi_v<float>;
    constexpr std::size_t angularSamples = 512;
    const auto radii = previewApertureRadii(blades, curvature, rotationDegrees);
    PreviewApertureLut lut{};
    for (std::uint32_t y = 0; y < previewApertureSize; ++y)
        for (std::uint32_t x = 0; x < previewApertureSize; ++x)
            for (int sample = 0; sample < 4; ++sample) {
                const float px = (static_cast<float>(x) + 0.25F + 0.5F * static_cast<float>(sample & 1)) *
                                     (2.0F / static_cast<float>(previewApertureSize)) -
                                 1.0F;
                const float py = (static_cast<float>(y) + 0.25F + 0.5F * static_cast<float>(sample >> 1)) *
                                     (2.0F / static_cast<float>(previewApertureSize)) -
                                 1.0F;
                const float angle = (std::atan2(py, px) / tau + 1.0F) * static_cast<float>(angularSamples);
                const auto index = static_cast<std::size_t>(angle);
                const float radius = std::lerp(radii[index % angularSamples], radii[(index + 1) % angularSamples],
                                               angle - std::floor(angle));
                if (px * px + py * py <= radius * radius)
                    lut[static_cast<std::size_t>(y) * previewApertureSize + x] += 0.25F;
            }
    return lut;
}

inline std::vector<std::array<float, 2>> previewApertureSamples(std::uint32_t blades, float curvature,
                                                                float rotationDegrees, std::uint32_t count) {
    const auto radii = previewApertureRadii(blades, curvature, rotationDegrees);
    auto cumulative = radii;
    for (std::size_t i = 0; i < radii.size(); ++i)
        cumulative[i] = (radii[i] * radii[i] + radii[(i + 1) % radii.size()] * radii[(i + 1) % radii.size()]) * 0.5F;
    std::partial_sum(cumulative.begin(), cumulative.end(), cumulative.begin());
    std::vector<std::array<float, 2>> samples;
    samples.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        // A Vogel disk mapped through the aperture's angular area CDF. The
        // Jacobian R(theta)^2 preserves uniform density without row/column
        // quantization or rejection reducing the requested sample budget.
        const float turn = static_cast<float>(i) * 0.38196601125F;
        const float value = (turn - std::floor(turn)) * cumulative.back();
        const auto it = std::lower_bound(cumulative.begin(), cumulative.end(), value);
        const auto index = static_cast<std::size_t>(std::distance(cumulative.begin(), it));
        const float previous = index == 0 ? 0.0F : cumulative[index - 1];
        const float fraction = (value - previous) / std::max(cumulative[index] - previous, 0.0001F);
        const float angle = (static_cast<float>(index) + fraction) *
                            (2.0F * std::numbers::pi_v<float> / static_cast<float>(radii.size()));
        const float radius = std::lerp(radii[index], radii[(index + 1) % radii.size()], fraction) *
                             std::sqrt((static_cast<float>(i) + 0.5F) / static_cast<float>(count));
        samples.push_back({std::cos(angle) * radius, std::sin(angle) * radius});
    }
    return samples;
}

} // namespace dayo::graphics
