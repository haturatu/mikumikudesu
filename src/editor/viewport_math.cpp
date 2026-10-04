#include "editor/viewport_math.hpp"

#include <algorithm>
#include <cmath>

namespace dayo::editor {
namespace {
core::Float4 normalized(core::Float4 q) noexcept {
    float length = 0.0F;
    for (const auto value : q)
        length += value * value;
    if (!std::isfinite(length) || length < 1e-12F)
        return {0.0F, 0.0F, 0.0F, 1.0F};
    for (auto& value : q)
        value /= std::sqrt(length);
    return q;
}
} // namespace
core::Float3 convertModelPoint(core::Float3 point, const core::PreviewNormalization& from,
                               const core::PreviewNormalization& to) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis)
        point[axis] = to.center[axis] + (point[axis] - from.center[axis]) * from.scale / std::max(to.scale, 1.0e-8F);
    return point;
}

ScreenPoint projectToViewport(core::Float3 world, const std::array<float, 16>& vp, ScreenRect rect) noexcept {
    const auto component = [&](std::size_t axis) {
        return world[0] * vp[axis] + world[1] * vp[axis + 4] + world[2] * vp[axis + 8] + vp[axis + 12];
    };
    const float w = component(3);
    if (!std::isfinite(w) || w <= 1e-6F || rect.width <= 0.0F || rect.height <= 0.0F)
        return {};
    const float x = component(0) / w, y = component(1) / w, z = component(2) / w;
    return {rect.x + (x + 1.0F) * rect.width * 0.5F, rect.y + (1.0F - y) * rect.height * 0.5F, z,
            std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && x >= -1.0F && x <= 1.0F && y >= -1.0F &&
                y <= 1.0F && z >= 0.0F && z <= 1.0F};
}
core::Float4 multiplyRotation(core::Float4 a, core::Float4 b) noexcept {
    return normalized(
        {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
         a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]});
}
core::Float4 inverseRotation(core::Float4 q) noexcept {
    q = normalized(q);
    return {-q[0], -q[1], -q[2], q[3]};
}
core::Float3 rotatePoint(core::Float4 q, core::Float3 p) noexcept {
    q = normalized(q);
    const core::Float3 cross{q[1] * p[2] - q[2] * p[1], q[2] * p[0] - q[0] * p[2], q[0] * p[1] - q[1] * p[0]};
    return {p[0] + 2.0F * (q[3] * cross[0] + q[1] * cross[2] - q[2] * cross[1]),
            p[1] + 2.0F * (q[3] * cross[1] + q[2] * cross[0] - q[0] * cross[2]),
            p[2] + 2.0F * (q[3] * cross[2] + q[0] * cross[1] - q[1] * cross[0])};
}
std::array<float, 16> poseMatrix(core::Float3 p, core::Float4 q) noexcept {
    const auto x = rotatePoint(q, {1.0F, 0.0F, 0.0F}), y = rotatePoint(q, {0.0F, 1.0F, 0.0F}),
               z = rotatePoint(q, {0.0F, 0.0F, 1.0F});
    return {x[0], x[1], x[2], 0.0F, y[0], y[1], y[2], 0.0F, z[0], z[1], z[2], 0.0F, p[0], p[1], p[2], 1.0F};
}
core::Float4 matrixRotation(const std::array<float, 16>& m) noexcept {
    const float trace = m[0] + m[5] + m[10];
    core::Float4 q{};
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q = {(m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s, s * 0.25F};
    } else {
        std::size_t axis = 0;
        if (m[5] > m[0])
            axis = 1;
        if (m[10] > m[axis * 5])
            axis = 2;
        const auto next = (axis + 1) % 3, last = (axis + 2) % 3;
        const float s = std::sqrt(std::max(0.0F, 1.0F + m[axis * 5] - m[next * 5] - m[last * 5])) * 2.0F;
        if (s <= 1e-6F)
            return {0.0F, 0.0F, 0.0F, 1.0F};
        q[axis] = s * 0.25F;
        q[next] = (m[axis * 4 + next] + m[next * 4 + axis]) / s;
        q[last] = (m[axis * 4 + last] + m[last * 4 + axis]) / s;
        q[3] = (m[next * 4 + last] - m[last * 4 + next]) / s;
    }
    return normalized(q);
}
} // namespace dayo::editor
