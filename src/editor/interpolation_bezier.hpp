#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace dayo::editor::detail {

// Control points are [x1, y1, x2, y2] in VMD's 0..127 range.
// The curve is y(u), with u obtained by inverting x(u) = t.
[[nodiscard]] inline float evaluateVmdBezier(float t, const std::array<std::uint8_t, 4>& points) noexcept {
    if (t <= 0.0F)
        return 0.0F;
    if (t >= 1.0F)
        return 1.0F;

    const auto unit = [](std::uint8_t value) {
        return static_cast<float>(std::min<std::uint8_t>(value, 127)) / 127.0F;
    };
    const auto cubic = [](float u, float first, float second) {
        const float inverse = 1.0F - u;
        return 3.0F * inverse * inverse * u * first + 3.0F * inverse * u * u * second + u * u * u;
    };
    const float x1 = unit(points[0]);
    const float x2 = unit(points[2]);

    float low = 0.0F;
    float high = 1.0F;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const float u = (low + high) * 0.5F;
        if (cubic(u, x1, x2) < t)
            low = u;
        else
            high = u;
    }
    return cubic((low + high) * 0.5F, unit(points[1]), unit(points[3]));
}

} // namespace dayo::editor::detail
