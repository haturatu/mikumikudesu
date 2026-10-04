#include "graphics/subayai_environment.hpp"
#include "graphics/environment_cache.hpp"

#include "core/image_hdr.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dayo::graphics {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

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
        const auto sourceY = static_cast<std::uint32_t>(((2ULL * y + 1ULL) * image.height) / (2ULL * sampleHeight));
        const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(sampleHeight);
        const float theta = v * kPi;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (std::uint32_t x = 0; x < sampleWidth; ++x) {
            const auto sourceX = static_cast<std::uint32_t>(((2ULL * x + 1ULL) * image.width) / (2ULL * sampleWidth));
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(sampleWidth);
            const float phi = (u - 0.5F) * 2.0F * kPi; // Match atan2(z, x)/(2*pi) + 0.5 in conversion.
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

// Area filtering retains small bright emitters when reducing an HDR panorama.
// Work in linear light, apply the exposure multiplier to RGB only, and keep
// the upload bounded independently of the decoded source resolution.
[[nodiscard]] core::ImageData prepareEnvironmentSource(const core::ImageData& image, float exposure) {
    const auto height = std::min(image.height, NativeEnvironmentBackend::maxFaceSize * 2U);
    core::ImageData output{.width = height * 2U,
                           .height = height,
                           .channels = 4,
                           .type = core::PixelType::half16,
                           .space = core::ColorSpace::linear,
                           .bytes = {}};
    output.bytes.resize(output.pixelCount() * 4U * sizeof(std::uint16_t));
    const double scale = static_cast<double>(image.height) / height;
    for (std::uint32_t y = 0; y < height; ++y) {
        const double y0 = y * scale, y1 = (y + 1U) * scale;
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const double x0 = x * scale, x1 = (x + 1U) * scale;
            std::array<double, 4> sum{};
            for (auto sy = static_cast<std::uint32_t>(y0);
                 sy < std::min(image.height, static_cast<std::uint32_t>(std::ceil(y1))); ++sy) {
                const double wy = std::min(y1, static_cast<double>(sy + 1U)) - std::max(y0, static_cast<double>(sy));
                for (auto sx = static_cast<std::uint32_t>(x0);
                     sx < std::min(image.width, static_cast<std::uint32_t>(std::ceil(x1))); ++sx) {
                    const double wx =
                        std::min(x1, static_cast<double>(sx + 1U)) - std::max(x0, static_cast<double>(sx));
                    const auto offset = (static_cast<std::size_t>(sy) * image.width + sx) * 4U;
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        sum[channel] += readImageSample(image, offset + channel) * wx * wy;
                }
            }
            const auto offset = (static_cast<std::size_t>(y) * output.width + x) * 4U;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const float value =
                    static_cast<float>(sum[channel] / (scale * scale)) * (channel < 3 ? exposure : 1.0F);
                const auto half = core::floatToHalf(std::clamp(value, 0.0F, 65504.0F));
                std::memcpy(output.bytes.data() + (offset + channel) * sizeof(half), &half, sizeof(half));
            }
        }
    }
    return output;
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

void EnvironmentService::clear() noexcept {
    if (ready_ && backend_ != nullptr)
        backend_->reset();
    cached_ = {};
    ready_ = false;
    cubemap_ = {};
    prefiltered_ = {};
    sphericalHarmonics_ = {};
    skywalkerVersion_ = 0;
    typedResult_ = {};
    recordPending_ = false;
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
                         {1, DescriptorKind::storageImage, 1, ShaderStageMask::compute},
                         {2, DescriptorKind::sampler, 1, ShaderStageMask::compute}}};
}

DescriptorSetLayoutDesc nativeEnvironmentPrefilterLayout() noexcept {
    return {.bindings = {{0, DescriptorKind::sampledImage, 1, ShaderStageMask::compute},
                         {1, DescriptorKind::storageImage, 1, ShaderStageMask::compute},
                         {2, DescriptorKind::sampler, 1, ShaderStageMask::compute}}};
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
    return regenerateImage(desc, core::loadImageData(desc.source));
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
    if (!std::isfinite(desc.exposure) || desc.exposure < 0.0F)
        throw std::invalid_argument("native environment exposure must be a finite non-negative multiplier");
    const auto linear = core::convertImage(image, core::PixelType::half16, core::ColorSpace::linear);
    return regenerateLinear(desc, linear);
}

EnvironmentGpuResult NativeEnvironmentBackend::regenerateLinear(const EnvironmentDesc& desc,
                                                                const core::ImageData& image) {
    const auto source = prepareEnvironmentSource(image, desc.exposure);
    auto harmonics = projectSphericalHarmonics(image);
    for (auto& coefficient : harmonics)
        coefficient *= desc.exposure;
    Device* device = device_;
    reset();
    device_ = device;
    faceSize_ = std::min(std::max(image.height / 2U, 1U), std::min(maxFaceSize, device_->environmentFaceSizeLimit()));
    mipLevels_ = mipCount(faceSize_);
    const auto sourceHash = core::fnv1a64({reinterpret_cast<const char*>(source.bytes.data()), source.bytes.size()});
    std::string keyInput = core::toHex(sourceHash);
    keyInput += "|" + std::to_string(source.width) + "|" + std::to_string(source.height) + "|" +
                std::to_string(faceSize_) + "|" + DAYO_ENVIRONMENT_CACHE_VERSION;
    keyInput += "|" + std::to_string(image.width) + "|" + std::to_string(image.height) + "|" +
                core::toHex(core::fnv1a64({reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size()})) +
                "|" + std::to_string(std::bit_cast<std::uint32_t>(desc.exposure));
    cachePath_ = environmentCachePath(core::toHex(core::fnv1a64(keyInput)));
    const auto cached =
        cachePath_ ? readEnvironmentCache(*cachePath_, faceSize_, mipLevels_) : std::vector<std::uint8_t>{};
    cacheHit_ = !cached.empty();
    try {
        resources_.source = device_->createTextureEx({
            .dimension = TextureDimension::d2,
            .extent = {source.width, source.height, 1},
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
            .usage = ResourceUsage::storageReadWrite | ResourceUsage::sampledRead | ResourceUsage::transferSrc |
                     ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent,
        });
        resources_.prefiltered = device_->createTextureEx({
            .dimension = TextureDimension::cube,
            .extent = {faceSize_, faceSize_, 1},
            .format = PixelFormat::rgba16Float,
            .mipLevels = mipLevels_,
            .arrayLayers = 1,
            .usage = ResourceUsage::storageReadWrite | ResourceUsage::sampledRead | ResourceUsage::transferSrc |
                     ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent,
        });
        device_->uploadTextureEx(resources_.source, source.bytes, 0, 0);
        if (cacheHit_) {
            std::size_t offset = 0;
            const auto uploadFaces = [&](handles::TextureHandle texture, std::uint32_t mip) {
                const auto size = std::max(faceSize_ >> mip, 1U);
                const auto bytes = static_cast<std::size_t>(size) * size * 8U;
                for (std::uint32_t face = 0; face < 6; ++face) {
                    device_->uploadTextureEx(texture, std::span(cached).subspan(offset, bytes), mip, face);
                    offset += bytes;
                }
            };
            uploadFaces(resources_.cubemap, 0);
            for (std::uint32_t mip = 0; mip < mipLevels_; ++mip)
                uploadFaces(resources_.prefiltered, mip);
            log::info("Reused cached environment cubemap: ", faceSize_, "px");
        }
        resources_.conversionSampler = device_->createSamplerEx({.filter = SamplerFilter::linear,
                                                                 .addressU = SamplerAddressMode::repeat,
                                                                 .addressV = SamplerAddressMode::clampToEdge,
                                                                 .addressW = SamplerAddressMode::clampToEdge});
        const std::array conversion{
            DescriptorBindingEx{.slot = 0, .arrayElement = 0, .texture = resources_.source},
            DescriptorBindingEx{.slot = 1, .arrayElement = 0, .texture = resources_.cubemap},
            DescriptorBindingEx{.slot = 2, .arrayElement = 0, .sampler = resources_.conversionSampler},
        };
        resources_.equirectToCubeSet = device_->allocateDescriptorSetEx(bindings_.equirectToCubeLayout, conversion);
        resources_.prefilterSampler = device_->createSamplerEx({.filter = SamplerFilter::linear,
                                                                .addressU = SamplerAddressMode::clampToEdge,
                                                                .addressV = SamplerAddressMode::clampToEdge,
                                                                .addressW = SamplerAddressMode::clampToEdge});
        resources_.prefilterSets.reserve(mipLevels_);
        for (std::uint32_t mip = 0; mip < mipLevels_; ++mip) {
            const std::array prefilter{
                DescriptorBindingEx{.slot = 0, .arrayElement = 0, .texture = resources_.cubemap},
                DescriptorBindingEx{.slot = 1, .arrayElement = 0, .texture = resources_.prefiltered, .mipLevel = mip},
                DescriptorBindingEx{.slot = 2, .arrayElement = 0, .sampler = resources_.prefilterSampler},
            };
            resources_.prefilterSets.push_back(device_->allocateDescriptorSetEx(bindings_.prefilterLayout, prefilter));
        }
        if (!resources_.equirectToCubeSet.valid() || !resources_.conversionSampler.valid() ||
            !resources_.prefilterSampler.valid() ||
            std::any_of(resources_.prefilterSets.begin(), resources_.prefilterSets.end(),
                        [](auto set) { return !set.valid(); }))
            throw std::runtime_error("native environment descriptor allocation returned an invalid handle");
    } catch (...) {
        reset();
        device_ = device;
        throw;
    }
    result_ = {.cubemap = resources_.cubemap,
               .prefiltered = resources_.prefiltered,
               .prefilteredMipLevels = mipLevels_,
               .sphericalHarmonics = harmonics,
               .skywalkerVersion = desc.version,
               .skybox = resources_.source};
    return result_;
}

void NativeEnvironmentBackend::record(CommandList& commands) const {
    if (!ready() || !bindings_.valid())
        throw std::logic_error("native environment backend is not initialized");
    if (cacheHit_)
        return;
    recorded_ = true;
    const NativeEnvironmentPushConstants constants{.faceSize = faceSize_, .mipLevels = mipLevels_};
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
    for (std::uint32_t mip = 0; mip < mipLevels_; ++mip) {
        auto mipConstants = constants;
        mipConstants.mipLevel = mip;
        constexpr std::array<std::uint32_t, 5> samples{1, 64, 48, 32, 16};
        mipConstants.sampleCount = samples[std::min<std::size_t>(mip, samples.size() - 1)];
        const auto mipSize = std::max(faceSize_ >> mip, 1U);
        commands.bindDescriptorSetEx(resources_.prefilterSets[mip]);
        commands.pushConstantsEx(std::as_bytes(std::span<const NativeEnvironmentPushConstants>(&mipConstants, 1)));
        commands.dispatch((mipSize + 7U) / 8U, (mipSize + 7U) / 8U, 6);
        commands.memoryBarrierEx();
    }
}

void NativeEnvironmentBackend::reset() noexcept {
    Device* device = device_;
    const bool hasResources = resources_.conversionSampler.valid() || !resources_.prefilterSets.empty() ||
                              resources_.prefilterSampler.valid() || resources_.equirectToCubeSet.valid() ||
                              resources_.prefiltered.valid() || resources_.cubemap.valid() || resources_.source.valid();
    if (device != nullptr && hasResources) {
        if (recorded_ && !cacheHit_ && cachePath_) {
            try {
                std::vector<TextureReadbackRequest> requests;
                requests.reserve(6U * (mipLevels_ + 1U));
                const auto appendFaces = [&](handles::TextureHandle texture, std::uint32_t mip) {
                    for (std::uint32_t face = 0; face < 6; ++face)
                        requests.push_back({texture, mip, face});
                };
                appendFaces(resources_.cubemap, 0);
                for (std::uint32_t mip = 0; mip < mipLevels_; ++mip)
                    appendFaces(resources_.prefiltered, mip);
                const auto bytes = device->readbackTextureSubresources(requests);
                writeEnvironmentCache(*cachePath_, faceSize_, mipLevels_, bytes);
            } catch (const std::exception& exception) {
                log::warn("Environment cache persistence failed: ", exception.what());
            }
        }
        for (auto set : resources_.prefilterSets) {
            if (set.valid()) {
                try {
                    device->destroyDescriptorSetEx(set);
                } catch (...) {
                }
            }
        }
        if (resources_.equirectToCubeSet.valid()) {
            try {
                device->destroyDescriptorSetEx(resources_.equirectToCubeSet);
            } catch (...) {
            }
        }
        for (const auto sampler : {resources_.prefilterSampler, resources_.conversionSampler}) {
            if (sampler.valid()) {
                try {
                    device->destroySamplerEx(sampler);
                } catch (...) {
                }
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
    cachePath_.reset();
    cacheHit_ = false;
    recorded_ = false;
    faceSize_ = 0;
    mipLevels_ = 0;
    device_ = nullptr;
}

} // namespace dayo::graphics
