#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dayo::app {

// Viewport mouse gestures match vmd-trace's App::drawViewport().
// Scene cameras are normalized in mikumikudesu, whereas vmd-trace uses
// unnormalized MMD coordinates. Pan and zoom convert between those units.
struct ViewportNavigation {
    static constexpr float orbitRadiansPerPixel = 0.008F;
    static constexpr float panUnitsPerPixel = 0.025F;
    static constexpr float zoomUnitsPerWheelTick = 2.0F;
    static constexpr float maximumPitch = 1.4F;
    static constexpr float minimumDistance = 3.0F;
    static constexpr float maximumDistance = 150.0F;

    [[nodiscard]] static float safeScale(float sceneScale) noexcept {
        return std::isfinite(sceneScale) ? std::max(std::abs(sceneScale), 0.0001F) : 1.0F;
    }

    static void orbit(float& yaw, float& pitch, float deltaX, float deltaY) noexcept {
        yaw += deltaX * orbitRadiansPerPixel;
        pitch = std::clamp(pitch + deltaY * orbitRadiansPerPixel, -maximumPitch, maximumPitch);
    }

    static void pan(std::array<float, 3>& target, float deltaX, float deltaY, float sceneScale) noexcept {
        const auto scale = safeScale(sceneScale);
        target[0] -= deltaX * panUnitsPerPixel * scale;
        target[1] += deltaY * panUnitsPerPixel * scale;
    }

    static void zoom(float& positiveDistance, float mouseWheel, float sceneScale) noexcept {
        const auto scale = safeScale(sceneScale);
        // vmd-trace stores camera distance as a negative number.
        const float traceDistance = -positiveDistance / scale;
        positiveDistance = -std::clamp(traceDistance + mouseWheel * zoomUnitsPerWheelTick,
                                       -maximumDistance, -minimumDistance) * scale;
    }
};

} // namespace dayo::app
