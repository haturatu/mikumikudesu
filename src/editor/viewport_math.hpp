#pragma once

#include "core/model_probe.hpp"

#include <array>

namespace dayo::editor {

struct ScreenPoint {
    float x{};
    float y{};
    float depth{};
    bool visible{};
};
struct ScreenRect {
    float x{};
    float y{};
    float width{};
    float height{};
};
[[nodiscard]] ScreenPoint projectToViewport(core::Float3 world, const std::array<float, 16>& vp,
                                            ScreenRect rect) noexcept;
[[nodiscard]] core::Float4 multiplyRotation(core::Float4 a, core::Float4 b) noexcept;
[[nodiscard]] core::Float4 inverseRotation(core::Float4 q) noexcept;
[[nodiscard]] core::Float3 rotatePoint(core::Float4 rotation, core::Float3 point) noexcept;
[[nodiscard]] std::array<float, 16> poseMatrix(core::Float3 position, core::Float4 rotation) noexcept;
[[nodiscard]] core::Float4 matrixRotation(const std::array<float, 16>& matrix) noexcept;

} // namespace dayo::editor
