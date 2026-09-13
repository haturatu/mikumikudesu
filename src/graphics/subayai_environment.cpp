#include "graphics/subayai_environment.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dayo::graphics {
namespace {

constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] float readImageSample(const core::ImageData& image, std::size_t sample) {
    if (image.type == core::PixelType::unorm8)
        return static_cast<float>(image.bytes[sample]);
    if (image.type == core::PixelType::half16) {
        std::uint16_t bits{};
        std::memcpy(&bits, image.bytes.data() + sample * sizeof(bits), sizeof(bits));
        return core::halfToFloat(bits);
    }
    float value{};
    std::memcpy(&value, image.bytes.data() + sample * sizeof(value), sizeof(value));
    return value;
}

[[nodiscard]] std::array<float, 9> shBasis(float x, float y, float z) noexcept {
    return {0.2820947918F,
            0.4886025119F * y,
            0.4886025119F * z,
            0.4886025119F * x,
            1.0925484306F * x * y,
            1.0925484306F * y * z,
            0.3153915653F * (3.0F * z * z - 1.0F),
            1.0925484306F * x * z,
            0.5462742153F * (x * x - y * y)};
}

[[nodiscard]] std::array<float, 27> projectSphericalHarmonics(const core::ImageData& image) {
    std::array<float, 27> result{};
    const auto sampleWidth = std::min<std::uint32_t>(image.width, 256U);
    const auto sampleHeight = std::min<std::uint32_t>(image.height, 128U);
    double totalWeight = 0.0;
    for (std::uint32_t y = 0; y < sampleHeight; ++y) {
        const auto sourceY = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * image.height) / sampleHeight);
        const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(sampleHeight);
        const float theta = v * kPi;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (std::uint32_t x = 0; x < sampleWidth; ++x) {
            const auto sourceX = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * image.width) / sampleWidth);
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(sampleWidth);
            const float phi = u * 2.0F * kPi;
            const float directionX = sinTheta * std::cos(phi);
            const float directionZ = sinTheta * std::sin(phi);
            const auto basis = shBasis(directionX, cosTheta, directionZ);
            const auto pixel = (static_cast<std::size_t>(sourceY) * image.width + sourceX) * image.channels;
            const float weight = std::max(sinTheta, 0.000001F);
            totalWeight += weight;
            for (std::size_t coefficient = 0; coefficient < basis.size(); ++coefficient) {
                for (std::uint32_t channel = 0; channel < 3; ++channel)
                    result[coefficient * 3U + channel] +=
                        readImageSample(image, pixel + channel) * basis[coefficient] * weight;
            }
        }
    }
    if (totalWeight > 0.0) {
        const float normalization = static_cast<float>(4.0 * kPi / totalWeight);
        for (auto& value : result)
            value *= normalization;
    }
    return result;
}

[[nodiscard]] std::uint32_t mipCount(std::uint32_t dimension) noexcept {
    std::uint32_t count = 1;
    while (dimension > 1) {
        dimension = std::max(dimension / 2U, 1U);
        ++count;
    }
    return count;
}

[[nodiscard]] std::array<DescriptorBindingEx, 2> passBindings(handles::TextureHandle source,
                                                               handles::TextureHandle destination) noexcept {
    return {DescriptorBindingEx{.slot = 0, .arrayElement = 0, .texture = source},
            DescriptorBindingEx{.slot = 1, .arrayElement = 0, .texture = destination}};
}

} // namespace

bool EnvironmentService::update(const EnvironmentDesc& desc) {
    if (ready_ && cached_ == desc) {
        log::debug("Environment unchanged; reusing cubemap/prefiltered/SH/Skywalker");
        return false;
    }
    const auto result = backend_ == nullptr ? EnvironmentGpuResult{} : backend_->regenerateEx(desc);
    cached_ = desc;
    ready_ = true;
    ++generations_;
    skywalkerVersion_ = desc.version;
    typedResult_ = result;
    if (typedResult_.skywalkerVersion == 0)
        typedResult_.skywalkerVersion = desc.version;
    sphericalHarmonics_ = typedResult_.sphericalHarmonics;
    recordPending_ = true;
    log::info("Environment regenerated: ", desc.source, " exposure ", desc.exposure);
    return true;
}

void EnvironmentService::setHandles(TextureHandle cubemap, TextureHandle prefiltered,
                                    std::uint64_t skywalkerVersion) noexcept {
    cubemap_ = cubemap;
    prefiltered_ = prefiltered;
    skywalkerVersion_ = skywalkerVersion;
    typedResult_.cubemap = {};
    typedResult_.prefiltered = {};
    typedResult_.sphericalHarmonics = sphericalHarmonics_;
    typedResult_.skywalkerVersion = skywalkerVersion;
    recordPending_ = false;
}

void EnvironmentService::setGpuResult(EnvironmentGpuResult result) noexcept {
    cubemap_ = {};
    prefiltered_ = {};
    typedResult_ = result;
    sphericalHarmonics_ = result.sphericalHarmonics;
    skywalkerVersion_ = result.skywalkerVersion;
    recordPending_ = false;
}

void EnvironmentService::record(CommandList& commands) const {
    if (!recordPending_)
        return;
    if (backend_ != nullptr)
        backend_->record(commands);
    recordPending_ = false;
}

DescriptorSetLayoutDesc nativeEnvironmentPassLayout() noexcept {
    return {.bindings = {{0, DescriptorKind::sampledImage, 1, ShaderStageMask::compute},
                         {1, DescriptorKind::storageImage, 1, ShaderStageMask::compute}}};
}

NativeEnvironmentBackend::~NativeEnvironmentBackend() {
    reset();
}

void NativeEnvironmentBackend::regenerate(const EnvironmentDesc& desc) {
    static_cast<void>(regenerateEx(desc));
}

EnvironmentGpuResult NativeEnvironmentBackend::regenerateEx(const EnvironmentDesc& desc) {
    if (device_ == nullptr)
        throw std::logic_error("native environment backend has no device");
    if (!bindings_.valid())
        throw std::invalid_argument("native environment backend has incomplete compute pass bindings");
    if (desc.source.empty())
        throw std::invalid_argument("native environment requires an equirectangular source");
    const auto ldr = core::loadImageRgba8(desc.source);
    core::ImageData image{.width = ldr.width,
                          .height = ldr.height,
                          .channels = 4,
                          .type = core::PixelType::unorm8,
                          .space = core::ColorSpace::srgb,
                          .bytes = ldr.pixels};
    return regenerateImage(desc, image);
}

EnvironmentGpuResult NativeEnvironmentBackend::regenerateImage(const EnvironmentDesc& desc,
                                                               const core::ImageData& image) {
    if (device_ == nullptr)
        throw std::logic_error("native environment backend has no device");
    if (!bindings_.valid())
        throw std::invalid_argument("native environment backend has incomplete compute pass bindings");
    if (image.width == 0 || image.height < 2 || image.channels != 4)
        throw std::invalid_argument("native environment requires a non-empty RGBA image");
    if (static_cast<std::uint64_t>(image.height) * 2U != image.width)
        throw std::invalid_argument("native environment source must have a 2:1 equirectangular aspect ratio");
    const auto linear = core::convertImage(image, core::PixelType::half16, core::ColorSpace::linear);
    return regenerateLinear(desc, linear);
}

EnvironmentGpuResult NativeEnvironmentBackend::regenerateLinear(const EnvironmentDesc& desc,
                                                                const core::ImageData& image) {
    Device* device = device_;
    reset();
    device_ = device;
    faceSize_ = std::max(image.height / 2U, 1U);
    mipLevels_ = mipCount(faceSize_);
    const auto harmonics = projectSphericalHarmonics(image);
    try {
        resources_.source = device_->createTextureEx({
            .dimension = TextureDimension::d2,
            .extent = {image.width, image.height, 1},
            .format = PixelFormat::rgba16Float,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent,
        });
        resources_.cubemap = device_->createTextureEx({
            .dimension = TextureDimension::cube,
            .extent = {faceSize_, faceSize_, 1},
            .format = PixelFormat::rgba16Float,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = ResourceUsage::storageReadWrite | ResourceUsage::sampledRead,
            .lifetime = ResourceLifetime::persistent,
        });
        resources_.prefiltered = device_->createTextureEx({
            .dimension = TextureDimension::cube,
            .extent = {faceSize_, faceSize_, 1},
            .format = PixelFormat::rgba16Float,
            .mipLevels = mipLevels_,
            .arrayLayers = 1,
            .usage = ResourceUsage::storageReadWrite | ResourceUsage::sampledRead,
            .lifetime = ResourceLifetime::persistent,
        });
        device_->uploadTextureEx(resources_.source, image.bytes, 0, 0);
        const auto conversion = passBindings(resources_.source, resources_.cubemap);
        resources_.equirectToCubeSet =
            device_->allocateDescriptorSetEx(bindings_.equirectToCubeLayout, conversion);
        const auto prefilter = passBindings(resources_.cubemap, resources_.prefiltered);
        resources_.prefilterSet = device_->allocateDescriptorSetEx(bindings_.prefilterLayout, prefilter);
        if (!resources_.equirectToCubeSet.valid() || !resources_.prefilterSet.valid())
            throw std::runtime_error("native environment descriptor allocation returned an invalid handle");
    } catch (...) {
        reset();
        device_ = device;
        throw;
    }
    result_ = {.cubemap = resources_.cubemap,
               .prefiltered = resources_.prefiltered,
               .sphericalHarmonics = harmonics,
               .skywalkerVersion = desc.version};
    return result_;
}

void NativeEnvironmentBackend::record(CommandList& commands) const {
    if (!ready() || !bindings_.valid())
        throw std::logic_error("native environment backend is not initialized");
    const NativeEnvironmentPushConstants constants{
        .faceSize = faceSize_, .mipLevels = mipLevels_, .reserved = {0, 0}};
    const auto groups = (faceSize_ + 7U) / 8U;
    commands.transitionEx(resources_.source);
    commands.transitionEx(resources_.cubemap);
    commands.transitionEx(resources_.prefiltered);
    commands.bindPipelineEx(bindings_.equirectToCubePipeline);
    commands.bindDescriptorSetEx(resources_.equirectToCubeSet);
    commands.pushConstantsEx(std::as_bytes(std::span<const NativeEnvironmentPushConstants>(&constants, 1)));
    commands.dispatch(groups, groups, 6);
    commands.memoryBarrierEx();
    commands.bindPipelineEx(bindings_.prefilterPipeline);
    commands.bindDescriptorSetEx(resources_.prefilterSet);
    commands.pushConstantsEx(std::as_bytes(std::span<const NativeEnvironmentPushConstants>(&constants, 1)));
    commands.dispatch(groups, groups, 6);
    commands.memoryBarrierEx();
}

void NativeEnvironmentBackend::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        if (resources_.prefilterSet.valid()) {
            try {
                device->destroyDescriptorSetEx(resources_.prefilterSet);
            } catch (...) {
            }
        }
        if (resources_.equirectToCubeSet.valid()) {
            try {
                device->destroyDescriptorSetEx(resources_.equirectToCubeSet);
            } catch (...) {
            }
        }
        for (const auto texture : {resources_.prefiltered, resources_.cubemap, resources_.source}) {
            if (!texture.valid())
                continue;
            try {
                device->destroyTextureEx(texture);
            } catch (...) {
            }
        }
    }
    resources_ = {};
    result_ = {};
    faceSize_ = 0;
    mipLevels_ = 0;
    device_ = nullptr;
}

} // namespace dayo::graphics
