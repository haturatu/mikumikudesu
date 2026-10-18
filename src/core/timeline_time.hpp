#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace dayo::core {

// VMD and MikuMikuDayo keyframe indices use a fixed 30 fps timebase.
// A future variable-rate playback clock must convert explicitly at its edge.
inline constexpr double kMmdTimelineFramesPerSecond = 30.0;

// FX FRAME uses the start of the current frame (floor, including negatives).
// Clamp before integer conversion: floating-point -> integer is UB out of range.
[[nodiscard]] inline std::int64_t mmdFrameIndex(float frame) noexcept {
    if (!std::isfinite(frame))
        return 0;
    const double index = std::floor(static_cast<double>(frame));
    if (index >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    if (index <= static_cast<double>(std::numeric_limits<std::int64_t>::min()))
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(index);
}

} // namespace dayo::core
