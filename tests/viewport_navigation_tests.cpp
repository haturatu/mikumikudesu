#include "app/viewport_navigation.hpp"

#include <array>
#include <cmath>
#include <iostream>

namespace {

[[nodiscard]] bool nearlyEqual(float left, float right) {
    return std::abs(left - right) <= 0.00001F;
}

} // namespace

int main() {
    using dayo::app::ViewportNavigation;

    float yaw = 0.0F;
    float pitch = 0.0F;
    ViewportNavigation::orbit(yaw, pitch, 100.0F, 50.0F);
    if (!nearlyEqual(yaw, 0.8F) || !nearlyEqual(pitch, 0.4F))
        return 1;
    ViewportNavigation::orbit(yaw, pitch, 0.0F, 1000.0F);
    if (!nearlyEqual(pitch, 1.4F))
        return 2;
    ViewportNavigation::orbit(yaw, pitch, 0.0F, -1000.0F);
    if (!nearlyEqual(pitch, -1.4F))
        return 3;

    // Same movement as vmd-trace: right-drag 40px shifts the target by -1 MMD unit.
    std::array<float, 3> target{};
    ViewportNavigation::pan(target, 40.0F, 20.0F, 0.1F);
    if (!nearlyEqual(target[0], -0.1F) || !nearlyEqual(target[1], 0.05F))
        return 4;

    float distance = 3.0F; // Normalized -30 MMD units at scale 0.1.
    ViewportNavigation::zoom(distance, 1.0F, 0.1F);
    if (!nearlyEqual(distance, 2.8F))
        return 5;
    ViewportNavigation::zoom(distance, -1000.0F, 0.1F);
    if (!nearlyEqual(distance, 15.0F))
        return 6;
    ViewportNavigation::zoom(distance, 1000.0F, 0.1F);
    if (!nearlyEqual(distance, 0.3F))
        return 7;

    yaw = 2.0F;
    pitch = 1.0F;
    const std::array<float, 3> center{0.0F, 10.0F, 0.0F};
    ViewportNavigation::reset(target, yaw, pitch, distance, center, 0.1F);
    if (!nearlyEqual(target[0], 0.0F) || !nearlyEqual(target[1], 0.0F) ||
        !nearlyEqual(yaw, 0.0F) || !nearlyEqual(pitch, 0.0F) || !nearlyEqual(distance, 3.0F))
        return 8;

    std::cout << "Viewport navigation matches vmd-trace gestures\n";
    return 0;
}
