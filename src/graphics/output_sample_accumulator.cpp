#include "graphics/output_sample_accumulator.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

[[nodiscard]] std::size_t rgba16fByteCount(Extent3D extent) {
    if (extent.width == 0 || extent.height == 0 || extent.depth != 1)
        throw std::invalid_argument("RGBA16F sample extent must be a non-empty 2D image");
    const auto pixels = static_cast<std::uint64_t>(extent.width) * extent.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / (4U * sizeof(std::uint16_t)))
        throw std::overflow_error("RGBA16F sample extent is too large");
    return static_cast<std::size_t>(pixels) * 4U * sizeof(std::uint16_t);
}

TextureResourceDesc resolvedTextureDesc(Extent3D extent) {
    return {.dimension = TextureDimension::d2,
            .extent = extent,
            .format = PixelFormat::rgba16Float,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = ResourceUsage::sampledRead | ResourceUsage::transferSrc | ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent};
}

} // namespace

void Rgba16fSampleAccumulator::begin(Extent3D extent, std::uint32_t sampleCount) {
    if (sampleCount == 0)
        throw std::invalid_argument("RGBA16F sample count must be non-zero");
    const auto byteCount = rgba16fByteCount(extent);
    const auto channels = byteCount / sizeof(std::uint16_t);
    if (channels > std::numeric_limits<std::size_t>::max() / sizeof(float))
        throw std::overflow_error("RGBA16F accumulator is too large");
    extent_ = extent;
    expectedSamples_ = sampleCount;
    receivedSamples_ = 0;
    sum_.assign(channels, 0.0F);
}

void Rgba16fSampleAccumulator::add(std::span<const std::uint8_t> rgba16f) {
    if (expectedSamples_ == 0)
        throw std::logic_error("RGBA16F accumulator has not been started");
    if (receivedSamples_ >= expectedSamples_)
        throw std::logic_error("RGBA16F accumulator received too many samples");
    if (rgba16f.size() != sum_.size() * sizeof(std::uint16_t))
        throw std::invalid_argument("RGBA16F sample byte count does not match its extent");

    for (std::size_t channel = 0; channel < sum_.size(); ++channel) {
        std::uint16_t half{};
        std::memcpy(&half, rgba16f.data() + channel * sizeof(half), sizeof(half));
        sum_[channel] += core::halfToFloat(half);
    }
    ++receivedSamples_;
}

std::vector<std::uint8_t> Rgba16fSampleAccumulator::resolve() const {
    if (!complete())
        throw std::logic_error("RGBA16F accumulator cannot resolve before all samples arrive");
    std::vector<std::uint8_t> result(sum_.size() * sizeof(std::uint16_t));
    const auto scale = 1.0F / static_cast<float>(expectedSamples_);
    for (std::size_t channel = 0; channel < sum_.size(); ++channel) {
        const auto half = core::floatToHalf(sum_[channel] * scale);
        std::memcpy(result.data() + channel * sizeof(half), &half, sizeof(half));
    }
    return result;
}

OutputSampleAccumulator::~OutputSampleAccumulator() {
    reset();
}

std::optional<NativeFrameOutput> OutputSampleAccumulator::addSample(Device& device, CommandList& commands,
                                                                    const NativeFrameOutput& sample,
                                                                    std::uint32_t sampleIndex,
                                                                    std::uint32_t sampleCount) {
    if (device_ != nullptr && device_ != &device)
        reset();
    if (sampleCount < 2 || sampleIndex >= sampleCount)
        throw std::invalid_argument("output sample index/count is invalid");
    if (!sample.valid() || sample.format != PixelFormat::rgba16Float)
        throw std::invalid_argument("output sample accumulation requires a valid RGBA16F image");

    if (sampleIndex == 0) {
        ensureTexture(device, sample.extent);
        samples_.begin(sample.extent, sampleCount);
        activeSampleCount_ = sampleCount;
        nextSampleIndex_ = 0;
    } else if (activeSampleCount_ != sampleCount || nextSampleIndex_ != sampleIndex ||
               extent_.width != sample.extent.width || extent_.height != sample.extent.height ||
               extent_.depth != sample.extent.depth) {
        throw std::logic_error("output samples must be sequential and use a consistent extent");
    }

    commands.flushAndWaitForHostReadbackEx();
    const auto bytes = device.readbackTextureEx(sample.texture, 0, 0);
    samples_.add(bytes);
    ++nextSampleIndex_;
    if (sampleIndex + 1U != sampleCount)
        return std::nullopt;

    if (!samples_.complete())
        throw std::logic_error("output sample sequence ended before the accumulator was complete");
    const auto resolvedBytes = samples_.resolve();
    device.uploadTextureEx(resolved_, resolvedBytes, 0, 0);
    activeSampleCount_ = 0;
    nextSampleIndex_ = 0;
    return NativeFrameOutput{.texture = resolved_, .extent = extent_, .format = PixelFormat::rgba16Float};
}

void OutputSampleAccumulator::ensureTexture(Device& device, Extent3D extent) {
    if (device_ != nullptr && device_ != &device)
        reset();
    if (resolved_.valid() && extent_.width == extent.width && extent_.height == extent.height &&
        extent_.depth == extent.depth) {
        device_ = &device;
        return;
    }
    if (resolved_.valid()) {
        try {
            device.waitIdle();
        } catch (...) {
        }
        device.destroyTextureEx(resolved_);
    }
    device_ = &device;
    extent_ = extent;
    resolved_ = device.createTextureEx(resolvedTextureDesc(extent));
    if (!resolved_.valid())
        throw std::runtime_error("output sample accumulator texture allocation failed");
}

void OutputSampleAccumulator::cancel() noexcept {
    activeSampleCount_ = 0;
    nextSampleIndex_ = 0;
}

void OutputSampleAccumulator::reset() noexcept {
    cancel();
    if (device_ != nullptr && resolved_.valid()) {
        try {
            device_->waitIdle();
            device_->destroyTextureEx(resolved_);
        } catch (...) {
        }
    }
    device_ = nullptr;
    extent_ = {};
    resolved_ = {};
    samples_ = {};
}

} // namespace dayo::graphics
