#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace dayo::graphics {

enum class PreviewFocusMode : std::uint8_t { cameraTarget, manual, bone, face, eyes, viewportPoint, trackedSurface };
enum class PreviewDofQuality : std::uint8_t { automatic, performance, balanced, high };

struct PreviewDofSettings {
    std::string boneName;
    std::array<float, 3> worldPoint{};
    std::array<float, 3> localOffsetMetres{};
    std::array<float, 3> faceOffsetMetres{0.0F, 0.1F, -0.06F};
    float fNumber{2.8F};
    bool temporal{true};
    float apertureCurvature{1.0F};
    float apertureRotationDegrees{};
    float maxRadiusPixels{};          // 1080p radius; zero inherits the quality cap.
    std::uint32_t cinematicSamples{}; // Zero disables image-export lens accumulation.
    std::uint32_t apertureBlades{};   // Zero is circular; otherwise 5..12.
    float sensorHeightMm{24.0F};
    // Each model is normalized to preview coordinates. A two-unit character
    // is 1.6 m tall at the default scale; projects can calibrate this explicitly.
    float metresPerUnit{0.8F};
    float focusDistanceMetres{2.4F};
    float focusOffsetMetres{};
    float focusResponseSeconds{0.12F};
    float focusDeadbandMetres{0.003F};
    float focusMaxSpeed{10.0F};
    std::uint32_t modelIndex{}; // Project model order, never a transient ModelId.
    std::uint32_t trackingInstance{};
    PreviewFocusMode focusMode{PreviewFocusMode::cameraTarget};
    PreviewDofQuality quality{PreviewDofQuality::automatic};
};

inline PreviewDofSettings sanitizePreviewDofSettings(PreviewDofSettings settings) {
    const auto clamp = [](float value, float fallback, float low, float high) {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    settings.fNumber = clamp(settings.fNumber, 2.8F, 0.7F, 32.0F);
    settings.apertureCurvature = clamp(settings.apertureCurvature, 1.0F, 0.0F, 1.0F);
    settings.apertureRotationDegrees = clamp(settings.apertureRotationDegrees, 0.0F, -180.0F, 180.0F);
    settings.maxRadiusPixels = clamp(settings.maxRadiusPixels, 0.0F, 0.0F, 128.0F);
    if (settings.cinematicSamples != 0)
        settings.cinematicSamples = settings.cinematicSamples <= 64    ? 64U
                                    : settings.cinematicSamples <= 128 ? 128U
                                                                       : 256U;
    if (settings.apertureBlades != 0 && (settings.apertureBlades < 5 || settings.apertureBlades > 12))
        settings.apertureBlades = 0;
    settings.sensorHeightMm = clamp(settings.sensorHeightMm, 24.0F, 1.0F, 100.0F);
    settings.metresPerUnit = clamp(settings.metresPerUnit, 0.8F, 0.01F, 100.0F);
    settings.focusDistanceMetres = clamp(settings.focusDistanceMetres, 2.4F, 0.05F, 10000.0F);
    settings.focusOffsetMetres = clamp(settings.focusOffsetMetres, 0.0F, -100.0F, 100.0F);
    settings.focusResponseSeconds = clamp(settings.focusResponseSeconds, 0.12F, 0.0F, 5.0F);
    settings.focusDeadbandMetres = clamp(settings.focusDeadbandMetres, 0.003F, 0.0F, 0.25F);
    settings.focusMaxSpeed = clamp(settings.focusMaxSpeed, 10.0F, 0.05F, 1000.0F);
    for (auto& value : settings.localOffsetMetres)
        value = clamp(value, 0.0F, -100.0F, 100.0F);
    for (auto& value : settings.faceOffsetMetres)
        value = clamp(value, 0.0F, -100.0F, 100.0F);
    for (auto& value : settings.worldPoint)
        value = clamp(value, 0.0F, -10000.0F, 10000.0F);
    if (settings.focusMode > PreviewFocusMode::trackedSurface)
        settings.focusMode = PreviewFocusMode::cameraTarget;
    if (settings.quality > PreviewDofQuality::high)
        settings.quality = PreviewDofQuality::automatic;
    return settings;
}

struct PreviewFocusState {
    float currentMetres{};
    float targetMetres{};
    float velocity{};
    bool trackingValid{};
};

inline void updatePreviewFocusState(PreviewFocusState& state, float targetMetres, float deltaSeconds,
                                    const PreviewDofSettings& settings, bool trackingValid,
                                    bool reset = false) noexcept {
    const float target = std::isfinite(targetMetres) ? std::max(targetMetres, 0.01F) : 0.01F;
    state.targetMetres = target;
    if (reset || !state.trackingValid || !trackingValid || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0F ||
        deltaSeconds > 0.25F || settings.focusResponseSeconds <= 0.0F) {
        state.currentMetres = target;
        state.velocity = 0.0F;
    } else {
        const float error = target - state.currentMetres;
        const float response = settings.focusResponseSeconds / std::max(1.0F, std::abs(error) / 0.25F);
        const float alpha = -std::expm1(-deltaSeconds / std::max(response, 0.001F));
        const float travel = std::abs(error) <= settings.focusDeadbandMetres
                                 ? 0.0F
                                 : std::clamp(error * alpha, -settings.focusMaxSpeed * deltaSeconds,
                                              settings.focusMaxSpeed * deltaSeconds);
        state.currentMetres += travel;
        state.velocity = travel / deltaSeconds;
    }
    state.trackingValid = trackingValid;
}

struct PreviewFocusCamera {
    std::array<float, 3> rotation{};
    std::array<float, 3> target{};
    float distance{3.0F};
    float verticalFovRadians{0.785398163F};
    bool perspective{true};
};

// Exactly the Rx -> Ry -> Rz convention in preview.hlsl, +Z forward.
inline std::array<float, 3> previewFocusViewPoint(const PreviewFocusCamera& camera,
                                                  const std::array<float, 3>& point) noexcept {
    const float x = point[0] - camera.target[0];
    const float y = point[1] - camera.target[1];
    const float z = point[2] - camera.target[2];
    const float y1 = y * std::cos(camera.rotation[0]) - z * std::sin(camera.rotation[0]);
    const float z1 = y * std::sin(camera.rotation[0]) + z * std::cos(camera.rotation[0]);
    const float x2 = x * std::cos(camera.rotation[1]) + z1 * std::sin(camera.rotation[1]);
    const float z2 = -x * std::sin(camera.rotation[1]) + z1 * std::cos(camera.rotation[1]);
    return {x2 * std::cos(camera.rotation[2]) - y1 * std::sin(camera.rotation[2]),
            x2 * std::sin(camera.rotation[2]) + y1 * std::cos(camera.rotation[2]),
            z2 + std::max(std::abs(camera.distance), 0.1F)};
}

inline float previewViewDepth(float depth, bool perspective) noexcept {
    constexpr float nearPlane = 0.05F;
    constexpr float farPlane = 100.0F;
    return perspective ? farPlane * nearPlane / (farPlane - depth * (farPlane - nearPlane))
                       : nearPlane + depth * (farPlane - nearPlane);
}

// A pick uses the camera and pixel centre from the submitted frame, even when
// an animated camera has moved by the time its fence signals. Clear depth is
// deliberately rejected rather than turning the sky into a focus point.
inline std::optional<std::array<float, 3>>
previewFocusUnproject(const PreviewFocusCamera& camera, std::array<float, 2> uv, float aspect, float depth) noexcept {
    if (!std::isfinite(depth) || depth < 0.0F || depth >= 1.0F || !std::isfinite(aspect) || aspect <= 0.0F ||
        !std::isfinite(uv[0]) || !std::isfinite(uv[1]) || uv[0] < 0.0F || uv[0] >= 1.0F || uv[1] < 0.0F ||
        uv[1] >= 1.0F)
        return std::nullopt;
    const float z = previewViewDepth(depth, camera.perspective);
    const float extent = std::tan(std::clamp(camera.verticalFovRadians, 0.05F, 3.13F) * 0.5F) *
                         (camera.perspective ? z : std::max(std::abs(camera.distance), 0.1F));
    const float x = (2.0F * uv[0] - 1.0F) * aspect * extent;
    const float y = (1.0F - 2.0F * uv[1]) * extent;
    const float z0 = z - std::max(std::abs(camera.distance), 0.1F);
    const float x1 = x * std::cos(camera.rotation[2]) + y * std::sin(camera.rotation[2]);
    const float y1 = -x * std::sin(camera.rotation[2]) + y * std::cos(camera.rotation[2]);
    const float x2 = x1 * std::cos(camera.rotation[1]) - z0 * std::sin(camera.rotation[1]);
    const float z2 = x1 * std::sin(camera.rotation[1]) + z0 * std::cos(camera.rotation[1]);
    return std::array{x2 + camera.target[0],
                      y1 * std::cos(camera.rotation[0]) + z2 * std::sin(camera.rotation[0]) + camera.target[1],
                      -y1 * std::sin(camera.rotation[0]) + z2 * std::cos(camera.rotation[0]) + camera.target[2]};
}

struct PreviewDofLens {
    float focusMetres{};
    float cocScale{}; // radius in full-resolution pixels = scale * (focus / z - 1)
    float maxRadius{};
    std::uint32_t samples{};
};

inline PreviewDofLens previewDofLens(const PreviewDofSettings& settings, float focusDistanceUnits,
                                     float verticalFovRadians, std::uint32_t height,
                                     PreviewDofQuality quality) noexcept {
    const float sensor = settings.sensorHeightMm * 0.001F;
    const float focal = sensor / (2.0F * std::tan(std::clamp(verticalFovRadians, 0.05F, 3.13F) * 0.5F));
    const float focus =
        std::max(focal + 0.001F, focusDistanceUnits * settings.metresPerUnit + settings.focusOffsetMetres);
    const float scale =
        focal * focal / (2.0F * settings.fNumber * (focus - focal)) * static_cast<float>(height) / sensor;
    const bool performance = quality == PreviewDofQuality::performance;
    const bool high = quality == PreviewDofQuality::high;
    return {focus, scale,
            (settings.maxRadiusPixels > 0.0F ? settings.maxRadiusPixels
             : performance                   ? 20.0F
             : high                          ? 48.0F
                                             : 32.0F) *
                static_cast<float>(height) / 1080.0F,
            performance ? 12U
            : high      ? 32U
                        : 24U};
}

} // namespace dayo::graphics
