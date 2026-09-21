#include "graphics/camera_matrices.hpp"

#include <algorithm>
#include <cmath>

namespace dayo::graphics {
namespace {

using Matrix = std::array<float, 16>;

[[nodiscard]] Matrix multiply(const Matrix& left, const Matrix& right) noexcept {
    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t index = 0; index < 4; ++index)
                result[row * 4U + column] += left[row * 4U + index] * right[index * 4U + column];
        }
    }
    return result;
}

} // namespace

SceneCameraMatrices makeSceneCameraMatrices(const fx::FxCameraState& camera, std::uint32_t width,
                                            std::uint32_t height, const GraphicsConvention& convention) noexcept {
    SceneCameraMatrices result;
    const auto pitch = camera.rotation[0];
    const auto yaw = camera.rotation[1];
    const auto roll = camera.rotation[2];
    const auto sx = std::sin(pitch);
    const auto cx = std::cos(pitch);
    const auto sy = std::sin(yaw);
    const auto cy = std::cos(yaw);
    const auto sz = std::sin(roll);
    const auto cz = std::cos(roll);

    // Coefficients for the preview shader's Rx -> Ry -> Rz transform. The
    // matrix is laid out for HLSL row-vector multiplication.
    const auto ax = cz * cy;
    const auto bx = cz * sy * sx - sz * cx;
    const auto cxv = cz * sy * cx + sz * sx;
    const auto ay = sz * cy;
    const auto by = sz * sy * sx + cz * cx;
    const auto cyv = sz * sy * cx - cz * sx;
    const auto az = -sy;
    const auto bz = cy * sx;
    const auto czv = cy * cx;

    const auto tx = -(camera.position[0] * ax + camera.position[1] * bx + camera.position[2] * cxv);
    const auto ty = -(camera.position[0] * ay + camera.position[1] * by + camera.position[2] * cyv);
    const auto tz = -(camera.position[0] * az + camera.position[1] * bz + camera.position[2] * czv) +
                    std::max(std::abs(camera.distance), 0.1F);
    result.view = {ax, ay, az, 0.0F, bx, by, bz, 0.0F, cxv, cyv, czv, 0.0F, tx, ty, tz, 1.0F};

    const auto aspect = height == 0 ? 1.0F : static_cast<float>(width) / static_cast<float>(height);
    const auto fov = std::clamp(std::abs(camera.verticalFovRadians), 0.05F, 3.13F);
    const auto focal = 1.0F / std::tan(fov * 0.5F);
    const auto nearPlane = 0.05F;
    const auto farPlane = 100.0F;
    const auto depthScale = farPlane / (farPlane - nearPlane);
    const auto depthOffset = -farPlane * nearPlane / (farPlane - nearPlane);
    const auto ySign = convention.framebufferYFlip ? -1.0F : 1.0F;
    if (camera.perspective) {
        result.projection = {focal / std::max(aspect, 0.001F), 0.0F, 0.0F, 0.0F,
                             0.0F, ySign * focal, 0.0F, 0.0F,
                             0.0F, 0.0F, depthScale, 1.0F,
                             0.0F, 0.0F, depthOffset, 0.0F};
    } else {
        const auto halfHeight = std::max(std::abs(camera.distance), 0.1F) * std::tan(fov * 0.5F);
        const auto halfWidth = std::max(aspect * halfHeight, 0.001F);
        const auto inverseDepth = 1.0F / (farPlane - nearPlane);
        result.projection = {1.0F / halfWidth, 0.0F, 0.0F, 0.0F,
                             0.0F, ySign / halfHeight, 0.0F, 0.0F,
                             0.0F, 0.0F, inverseDepth, 0.0F,
                             0.0F, 0.0F, -nearPlane * inverseDepth, 1.0F};
    }
    result.viewProjection = multiply(result.view, result.projection);
    return result;
}

} // namespace dayo::graphics
