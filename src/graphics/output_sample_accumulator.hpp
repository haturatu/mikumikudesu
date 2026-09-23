#pragma once

#include "core/image_hdr.hpp"
#include "graphics/device.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dayo::graphics {

// CPU-side linear accumulator for native RGBA16F renderer outputs. It keeps
// HDR values in float32 and narrows only when the completed average is written
// back to the persistent GPU texture.
class Rgba16fSampleAccumulator {
  public:
    void begin(Extent3D extent, std::uint32_t sampleCount);
    void add(std::span<const std::uint8_t> rgba16f);
    [[nodiscard]] std::vector<std::uint8_t> resolve() const;

    [[nodiscard]] bool complete() const noexcept {
        return expectedSamples_ != 0 && receivedSamples_ == expectedSamples_;
    }

  private:
    Extent3D extent_{};
    std::uint32_t expectedSamples_{};
    std::uint32_t receivedSamples_{};
    std::vector<float> sum_;
};

// Image-sequence output needs to run nonlinear postprocess once over the
// average of all linear renderer samples. The Vulkan bridge reads native
// RGBA16F values (never the 8-bit presentation image), accumulates in float32,
// and uploads the completed average for the postprocess chain.
class OutputSampleAccumulator {
  public:
    OutputSampleAccumulator() = default;
    ~OutputSampleAccumulator();

    OutputSampleAccumulator(const OutputSampleAccumulator&) = delete;
    OutputSampleAccumulator& operator=(const OutputSampleAccumulator&) = delete;

    [[nodiscard]] std::optional<NativeFrameOutput> addSample(Device& device, CommandList& commands,
                                                             const NativeFrameOutput& sample, std::uint32_t sampleIndex,
                                                             std::uint32_t sampleCount);
    void cancel() noexcept;
    void reset() noexcept;

  private:
    void ensureTexture(Device& device, Extent3D extent);

    Device* device_{};
    Extent3D extent_{};
    handles::TextureHandle resolved_{};
    Rgba16fSampleAccumulator samples_;
    std::uint32_t activeSampleCount_{};
    std::uint32_t nextSampleIndex_{};
};

} // namespace dayo::graphics
