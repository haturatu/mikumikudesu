#include "graphics/native_dayo_environment_runtime.hpp"

#include "core/image_hdr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace dayo::graphics {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kMaxEnvironmentLuminance = 1.0e6F;
constexpr std::array<float, 3> kLuminanceWeights{0.2126729F, 0.7151522F, 0.0721750F};

struct WalkerWorker {
    std::uint32_t index{};
    float value{};
};

[[nodiscard]] float imageSample(const core::ImageData& image, std::size_t sample) {
    switch (image.type) {
    case core::PixelType::unorm8:
        return static_cast<float>(image.bytes[sample]) / 255.0F;
    case core::PixelType::half16: {
        std::uint16_t value{};
        std::memcpy(&value, image.bytes.data() + sample * sizeof(value), sizeof(value));
        return core::halfToFloat(value);
    }
    case core::PixelType::float32: {
        float value{};
        std::memcpy(&value, image.bytes.data() + sample * sizeof(value), sizeof(value));
        return value;
    }
    }
    throw std::invalid_argument("unsupported environment image pixel type");
}

[[nodiscard]] std::array<float, 4> linearSkyPixel(const core::ImageData& image, std::size_t pixel) {
    const auto base = pixel * image.channels;
    std::array<float, 4> result{0.0F, 0.0F, 0.0F, 1.0F};
    for (std::uint32_t channel = 0; channel < image.channels; ++channel)
        result[channel] = imageSample(image, base + channel);
    const float luminance =
        result[0] * kLuminanceWeights[0] + result[1] * kLuminanceWeights[1] + result[2] * kLuminanceWeights[2];
    if (!std::isfinite(luminance) || !std::ranges::all_of(result, [](float value) { return std::isfinite(value); }))
        throw std::invalid_argument("environment image contains non-finite pixels");
    if (luminance < 0.0F)
        throw std::invalid_argument("environment image contains negative luminance");
    if (luminance > kMaxEnvironmentLuminance) {
        const float scale = kMaxEnvironmentLuminance / luminance;
        result[0] *= scale;
        result[1] *= scale;
        result[2] *= scale;
    }
    return result;
}

[[nodiscard]] float cappedLuminance(const std::array<float, 4>& pixel) noexcept {
    return std::min(pixel[0] * kLuminanceWeights[0] + pixel[1] * kLuminanceWeights[1] + pixel[2] * kLuminanceWeights[2],
                    kMaxEnvironmentLuminance);
}

void buildAliasTable(std::span<const float> source, std::span<DayoWalkerAlias> destination) {
    if (source.empty() || source.size() != destination.size() ||
        source.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("invalid Dayo Walker alias table extent");

    const auto count = static_cast<std::uint32_t>(source.size());
    float sum = 0.0F;
    for (const auto value : source) {
        if (!std::isfinite(value) || value < 0.0F)
            throw std::invalid_argument("Dayo Walker source values must be finite and non-negative");
        sum += value;
    }
    const float average = sum / static_cast<float>(count);
    if (average == 0.0F) {
        const float probability = 1.0F / static_cast<float>(count);
        std::ranges::fill(destination, DayoWalkerAlias{.pair = UINT32_MAX, .probability = 1.0F, .pdf = probability});
        return;
    }

    std::vector<WalkerWorker> work(count);
    std::uint32_t small = 0;
    std::uint32_t large = count - 1U;
    for (std::uint32_t index = 0; index < count; ++index) {
        WalkerWorker worker{index, source[index] / average};
        if (worker.value <= 1.0F) {
            work[small] = worker;
            ++small;
        } else {
            work[large] = worker;
            --large;
        }
    }
    --small;
    ++large;

    while (small != UINT32_MAX) {
        if (large < count) {
            auto donor = work[large];
            const auto recipient = work[small];
            destination[recipient.index] = {
                .pair = donor.index, .probability = recipient.value, .pdf = source[recipient.index] / sum};
            donor.value -= 1.0F - recipient.value;
            if (donor.value <= 1.0F) {
                work[small] = donor;
                ++large;
            } else {
                work[large] = donor;
                --small;
            }
        } else {
            const auto worker = work[small];
            destination[worker.index] = {.pair = UINT32_MAX, .probability = 1.0F, .pdf = source[worker.index] / sum};
            --small;
        }
    }
    while (large < count) {
        const auto worker = work[large];
        destination[worker.index] = {.pair = UINT32_MAX, .probability = 1.0F, .pdf = source[worker.index] / sum};
        ++large;
    }
    for (std::uint32_t index = 0; index < count; ++index)
        destination[index].pdf = source[index] / sum;
}

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] handles::BufferHandle createAndUploadBuffer(Device& device, std::span<const std::byte> bytes) {
    if (bytes.empty())
        throw std::invalid_argument("cannot create an empty Dayo environment buffer");
    const auto buffer = device.createBufferEx({.size = bytes.size(),
                                               .usage = ResourceUsage::storageRead | ResourceUsage::hostRead,
                                               .cpuVisible = true,
                                               .lifetime = ResourceLifetime::persistent});
    if (!buffer.valid())
        throw std::runtime_error("Dayo environment buffer allocation returned an invalid handle");
    try {
        device.uploadBufferEx(buffer, bytes, 0);
    } catch (...) {
        device.destroyBufferEx(buffer);
        throw;
    }
    return buffer;
}

void destroyBuffer(Device* device, handles::BufferHandle& buffer) noexcept {
    if (device != nullptr && buffer.valid()) {
        try {
            device->destroyBufferEx(buffer);
        } catch (...) {
        }
    }
    buffer = {};
}

} // namespace

DayoEnvironmentCpuResources buildDayoEnvironmentCpuResources(const core::ImageData& linearImage,
                                                             bool buildSkyboxSampler) {
    if (linearImage.width == 0 || linearImage.height < 2 || linearImage.channels != 4 ||
        linearImage.space != core::ColorSpace::linear ||
        static_cast<std::uint64_t>(linearImage.height) * 2U != linearImage.width)
        throw std::invalid_argument("Dayo environment requires a linear, non-empty 2:1 RGBA image");
    const auto pixelCount64 = static_cast<std::uint64_t>(linearImage.width) * linearImage.height;
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max() ||
        pixelCount64 > std::numeric_limits<std::size_t>::max() / sizeof(DayoWalkerAlias))
        throw std::overflow_error("Dayo environment dimensions exceed Walker table limits");
    const auto pixelCount = static_cast<std::size_t>(pixelCount64);
    if (core::pixelByteSize(linearImage.type, linearImage.channels) >
            std::numeric_limits<std::size_t>::max() / pixelCount ||
        linearImage.bytes.size() < pixelCount * core::pixelByteSize(linearImage.type, linearImage.channels))
        throw std::invalid_argument("Dayo environment image byte storage is truncated");

    std::vector<std::array<float, 4>> pixels(pixelCount);
    std::vector<float> luminance(pixelCount);
    for (std::size_t index = 0; index < pixelCount; ++index) {
        pixels[index] = linearSkyPixel(linearImage, index);
        luminance[index] = cappedLuminance(pixels[index]);
    }

    DayoEnvironmentCpuResources result;
    if (buildSkyboxSampler) {
        result.skywalker.resize(pixelCount);
        result.skywalkerRow.resize(linearImage.height);
        std::vector<float> rowLuminance(linearImage.height);
        std::vector<float> rowPixels(linearImage.width);
        for (std::uint32_t y = 0; y < linearImage.height; ++y) {
            const auto rowOffset = static_cast<std::size_t>(y) * linearImage.width;
            std::copy_n(luminance.begin() + static_cast<std::ptrdiff_t>(rowOffset), linearImage.width,
                        rowPixels.begin());
            buildAliasTable(rowPixels,
                            std::span<DayoWalkerAlias>(result.skywalker).subspan(rowOffset, linearImage.width));
            float sum = 0.0F;
            for (const auto value : rowPixels)
                sum += value;
            rowLuminance[y] = sum / static_cast<float>(linearImage.width) *
                              std::sin(kPi * (static_cast<float>(y) + 0.5F) / static_cast<float>(linearImage.height));
        }
        buildAliasTable(rowLuminance, result.skywalkerRow);
    } else {
        result.skywalker.push_back({});
        result.skywalkerRow.push_back({});
    }

    auto& sh = result.skyboxSh.coefficients;
    for (std::uint32_t y = 0; y < linearImage.height; ++y) {
        const float theta = (static_cast<float>(y) + 0.5F) / static_cast<float>(linearImage.height) * kPi;
        const float cosTheta = std::cos(theta);
        const float sinTheta = std::sin(theta);
        std::array<std::array<float, 4>, 9> rowCoefficients{};
        for (std::uint32_t x = 0; x < linearImage.width; ++x) {
            const float phi = (static_cast<float>(x) + 0.5F) / static_cast<float>(linearImage.width) * 2.0F * kPi;
            const float nX = std::cos(phi) * sinTheta;
            const float nY = std::sin(phi) * sinTheta;
            const auto& color = pixels[static_cast<std::size_t>(y) * linearImage.width + x];
            const std::array<float, 9> basis{
                0.282095F,
                -0.488603F * nY,
                0.488603F * cosTheta,
                -0.488603F * nX,
                1.092548F * nX * nY,
                -1.092548F * nY * cosTheta,
                0.315392F * (3.0F * cosTheta * cosTheta - 1.0F),
                -1.092548F * nX * cosTheta,
                0.546274F * (nX * nX - nY * nY),
            };
            for (std::size_t coefficient = 0; coefficient < basis.size(); ++coefficient)
                for (std::size_t channel = 0; channel < color.size(); ++channel)
                    rowCoefficients[coefficient][channel] += color[channel] * basis[coefficient];
        }
        for (std::size_t coefficient = 0; coefficient < rowCoefficients.size(); ++coefficient) {
            for (auto& channel : rowCoefficients[coefficient])
                channel /= static_cast<float>(linearImage.width);
            for (std::size_t channel = 0; channel < rowCoefficients[coefficient].size(); ++channel)
                sh[coefficient][channel] += rowCoefficients[coefficient][channel] * sinTheta;
        }
    }
    const float shScale = 2.0F * kPi * kPi / static_cast<float>(linearImage.height);
    for (auto& coefficient : sh)
        for (auto& channel : coefficient)
            channel *= shScale;
    return result;
}

NativeDayoEnvironmentRuntime::~NativeDayoEnvironmentRuntime() {
    reset();
}

bool NativeDayoEnvironmentRuntime::sync(Device& device, handles::TextureHandle skybox,
                                        const std::filesystem::path& source, std::uint64_t sourceVersion,
                                        bool buildSkyboxSampler, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!skybox.valid() || source.empty()) {
        setError(error, "Dayo environment requires a real Skybox texture and source path");
        return false;
    }
    if (device_ != nullptr && device_ != &device) {
        setError(error, "Dayo environment resources belong to a different device");
        return false;
    }
    if (ready() && skybox_ == skybox && source_ == source && sourceVersion_ == sourceVersion &&
        buildSkyboxSampler_ == buildSkyboxSampler)
        return true;

    handles::BufferHandle newWalker{};
    handles::BufferHandle newWalkerRow{};
    handles::BufferHandle newSh{};
    try {
        auto sourceData = core::loadImageData(source);
        sourceData = core::convertImage(sourceData, core::PixelType::half16, core::ColorSpace::linear);
        const auto cpu = buildDayoEnvironmentCpuResources(sourceData, buildSkyboxSampler);
        newWalker = createAndUploadBuffer(device, std::as_bytes(std::span<const DayoWalkerAlias>(cpu.skywalker)));
        newWalkerRow = createAndUploadBuffer(device, std::as_bytes(std::span<const DayoWalkerAlias>(cpu.skywalkerRow)));
        newSh = createAndUploadBuffer(device, std::as_bytes(std::span<const DayoSphericalHarmonics>(&cpu.skyboxSh, 1)));
    } catch (const std::exception& exception) {
        destroyBuffer(&device, newSh);
        destroyBuffer(&device, newWalkerRow);
        destroyBuffer(&device, newWalker);
        setError(error, std::string("Dayo environment synchronization failed: ") + exception.what());
        return false;
    } catch (...) {
        destroyBuffer(&device, newSh);
        destroyBuffer(&device, newWalkerRow);
        destroyBuffer(&device, newWalker);
        setError(error, "Dayo environment synchronization failed");
        return false;
    }

    // The current scene descriptor sets may still be referenced by a submitted
    // frame. Keep their resources alive until that work has completed.
    try {
        device.waitIdle();
    } catch (const std::exception& exception) {
        destroyBuffer(&device, newSh);
        destroyBuffer(&device, newWalkerRow);
        destroyBuffer(&device, newWalker);
        setError(error, std::string("Dayo environment synchronization failed: ") + exception.what());
        return false;
    } catch (...) {
        destroyBuffer(&device, newSh);
        destroyBuffer(&device, newWalkerRow);
        destroyBuffer(&device, newWalker);
        setError(error, "Dayo environment synchronization failed while waiting for previous frames");
        return false;
    }

    destroyBuffer(device_, skyboxSh_);
    destroyBuffer(device_, skywalkerRow_);
    destroyBuffer(device_, skywalker_);
    device_ = &device;
    skybox_ = skybox;
    skywalker_ = newWalker;
    skywalkerRow_ = newWalkerRow;
    skyboxSh_ = newSh;
    source_ = source;
    sourceVersion_ = sourceVersion;
    buildSkyboxSampler_ = buildSkyboxSampler;
    ++generation_;
    return true;
}

void NativeDayoEnvironmentRuntime::apply(NativeSceneResourceBindings& bindings) const noexcept {
    // Empty handles intentionally clear prior bindings; the scene resource
    // store substitutes its placeholders and drops the host-resource bits.
    bindings.skybox = skybox_;
    bindings.skywalker = skywalker_;
    bindings.skywalkerRow = skywalkerRow_;
    bindings.skyboxSh = skyboxSh_;
}

void NativeDayoEnvironmentRuntime::reset() noexcept {
    Device* device = device_;
    const bool hasResources = skyboxSh_.valid() || skywalkerRow_.valid() || skywalker_.valid();
    if (device != nullptr && hasResources) {
        try {
            device->waitIdle();
        } catch (...) {
        }
    }
    destroyBuffer(device, skyboxSh_);
    destroyBuffer(device, skywalkerRow_);
    destroyBuffer(device, skywalker_);
    device_ = nullptr;
    skybox_ = {};
    source_.clear();
    sourceVersion_ = 0;
    buildSkyboxSampler_ = false;
}

} // namespace dayo::graphics
