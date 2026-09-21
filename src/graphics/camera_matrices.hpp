#pragma once

#include "fx/fx_frame.hpp"
#include "graphics/device.hpp"

namespace dayo::graphics {

struct SceneCameraMatrices {
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> viewProjection{};
};

// Builds the same left-handed, depth-zero-to-one camera contract used by the
// preview camera and by MikuMikuDayo's ViewCB. The application supplies the
// already-normalized target, Euler rotation, distance, and FOV so preview and
// native rendering cannot silently evaluate different camera poses.
[[nodiscard]] SceneCameraMatrices makeSceneCameraMatrices(const fx::FxCameraState& camera, std::uint32_t width,
                                                          std::uint32_t height,
                                                          const GraphicsConvention& convention) noexcept;

} // namespace dayo::graphics
