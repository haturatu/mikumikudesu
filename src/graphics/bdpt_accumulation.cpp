#include "graphics/bdpt_accumulation.hpp"

#include "core/log.hpp"
#include "core/scene.hpp"
#include "graphics/device.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace dayo::graphics {
namespace {

constexpr std::size_t kSpectralSamples = 64;
constexpr std::size_t kBlackbodySamples = 256;

float clampUnit(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

float gaussian(float value, float center, float width) noexcept {
    const float distance = (value - center) / width;
    return std::exp(-0.5F * distance * distance);
}

void buildSpectralLut(std::vector<float>& output) {
    output.resize(kSpectralSamples * 3U);
    for (std::size_t index = 0; index < kSpectralSamples; ++index) {
        const float wavelength =
            380.0F + 400.0F * static_cast<float>(index) / static_cast<float>(kSpectralSamples - 1U);
        output[index * 3U + 0U] = gaussian(wavelength, 610.0F, 42.0F);
        output[index * 3U + 1U] = gaussian(wavelength, 545.0F, 38.0F);
        output[index * 3U + 2U] = gaussian(wavelength, 455.0F, 28.0F);
    }
}

// Approximation of the Kelvin-to-RGB curve used for the host LUT. The native
// shader receives the resulting table, so it does not need to repeat the
// logarithmic branch logic for every sampled path.
std::array<float, 3> blackbodyRgb(float kelvin) noexcept {
    const float temperature = std::max(kelvin, 1000.0F) / 100.0F;
    float red = 255.0F;
    float green = 255.0F;
    float blue = 255.0F;
    if (temperature > 66.0F) {
        red = 329.6987F * std::pow(temperature - 60.0F, -0.13320476F);
        green = 288.12216F * std::pow(temperature - 60.0F, 0.07551485F);
        blue = 255.0F;
    } else {
        green = 99.4708F * std::log(temperature) - 161.11957F;
        if (temperature <= 19.0F) {
            blue = 0.0F;
        } else {
            blue = 138.51773F * std::log(temperature - 10.0F) - 305.0448F;
        }
    }
    return {clampUnit(red / 255.0F), clampUnit(green / 255.0F), clampUnit(blue / 255.0F)};
}

void buildBlackbodyLut(std::vector<float>& output) {
    output.resize(kBlackbodySamples * 3U);
    for (std::size_t index = 0; index < kBlackbodySamples; ++index) {
        const float kelvin =
            1000.0F + 14000.0F * static_cast<float>(index) / static_cast<float>(kBlackbodySamples - 1U);
        const auto rgb = blackbodyRgb(kelvin);
        std::copy(rgb.begin(), rgb.end(), output.begin() + static_cast<std::ptrdiff_t>(index * 3U));
    }
}

void setError(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

} // namespace

void BdptAccumulation::ensurePersistent() {
    if (persistentReady_) {
        log::debug("BDPT persistent resources reused");
        return;
    }
    buildSpectralLut(spectralLut_);
    buildBlackbodyLut(blackbodyLut_);
    for (std::size_t index = 0; index < volumes_.size(); ++index) {
        volumes_[index].handle = static_cast<std::uint64_t>(index + 1U);
        volumes_[index].valid = true;
    }
    spectralReady_ = true;
    blackbodyReady_ = true;
    persistentReady_ = true;
    ++generations_;
    log::info("BDPT persistent LUTs and 8 volume slots ready");
}

bool BdptAccumulation::ensureGpuResources(Device& device, std::uint32_t width, std::uint32_t height,
                                          std::uint32_t volumeResolution, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (width == 0 || height == 0 || volumeResolution == 0) {
        setError(error, "BDPT GPU resources require non-zero extents");
        return false;
    }
    ensurePersistent();
    if (gpuResources_.valid() && gpuDevice_ == &device && gpuResources_.width == width &&
        gpuResources_.height == height && gpuResources_.volumeResolution == volumeResolution)
        return true;
    if (gpuDevice_ != nullptr && gpuDevice_ != &device) {
        setError(error, "BDPT GPU resources belong to a different device");
        return false;
    }
    if (gpuDevice_ == &device)
        releaseGpuResourcesNoexcept(device);

    GpuResources created;
    created.width = width;
    created.height = height;
    created.volumeResolution = volumeResolution;
    const auto destroyCreated = [&]() noexcept {
        try {
            device.waitIdle();
            for (auto volume : created.volumes) {
                if (volume.valid())
                    device.destroyTextureEx(volume);
            }
            if (created.blackbodyLut.valid())
                device.destroyBufferEx(created.blackbodyLut);
            if (created.spectralLut.valid())
                device.destroyBufferEx(created.spectralLut);
            if (created.accumulation.valid())
                device.destroyTextureEx(created.accumulation);
        } catch (...) {
            // Destruction is best-effort during failure unwinding. The device
            // owns the final teardown order when it itself is destroyed.
        }
    };
    try {
        created.accumulation = device.createTextureEx({
            .dimension = TextureDimension::d2,
            .extent = {width, height, 1},
            .format = PixelFormat::rgba16Float,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = ResourceUsage::storageReadWrite | ResourceUsage::sampledRead | ResourceUsage::transferSrc |
                     ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent,
        });
        created.spectralLut = device.createBufferEx({
            .size = spectralLut_.size() * sizeof(float),
            .usage = ResourceUsage::storageRead | ResourceUsage::transferDst | ResourceUsage::hostRead,
            .cpuVisible = true,
            .lifetime = ResourceLifetime::persistent,
        });
        created.blackbodyLut = device.createBufferEx({
            .size = blackbodyLut_.size() * sizeof(float),
            .usage = ResourceUsage::storageRead | ResourceUsage::transferDst | ResourceUsage::hostRead,
            .cpuVisible = true,
            .lifetime = ResourceLifetime::persistent,
        });
        device.uploadBufferEx(created.spectralLut, std::as_bytes(std::span<const float>(spectralLut_)), 0);
        device.uploadBufferEx(created.blackbodyLut, std::as_bytes(std::span<const float>(blackbodyLut_)), 0);
        for (auto& volume : created.volumes) {
            volume = device.createTextureEx({
                .dimension = TextureDimension::d3,
                .extent = {volumeResolution, volumeResolution, volumeResolution},
                .format = PixelFormat::rgba16Float,
                .mipLevels = 1,
                .arrayLayers = 1,
                .usage = ResourceUsage::storageReadWrite | ResourceUsage::sampledRead | ResourceUsage::transferSrc |
                         ResourceUsage::transferDst,
                .lifetime = ResourceLifetime::persistent,
            });
        }
    } catch (const std::exception& exception) {
        destroyCreated();
        setError(error, std::string("BDPT GPU resource creation failed: ") + exception.what());
        return false;
    } catch (...) {
        destroyCreated();
        setError(error, "BDPT GPU resource creation failed");
        return false;
    }
    gpuResources_ = created;
    gpuDevice_ = &device;
    return true;
}

void BdptAccumulation::releaseGpuResources(Device& device) noexcept {
    if (gpuDevice_ == &device)
        releaseGpuResourcesNoexcept(device);
}

void BdptAccumulation::releaseGpuResourcesNoexcept(Device& device) noexcept {
    try {
        device.waitIdle();
        for (auto volume : gpuResources_.volumes) {
            if (volume.valid())
                device.destroyTextureEx(volume);
        }
        if (gpuResources_.blackbodyLut.valid())
            device.destroyBufferEx(gpuResources_.blackbodyLut);
        if (gpuResources_.spectralLut.valid())
            device.destroyBufferEx(gpuResources_.spectralLut);
        if (gpuResources_.accumulation.valid())
            device.destroyTextureEx(gpuResources_.accumulation);
    } catch (...) {
        // Device teardown is already noexcept; stale handles are discarded so
        // they cannot be reused after a failed cleanup.
    }
    gpuResources_ = {};
    gpuDevice_ = nullptr;
}

bool BdptAccumulation::beginFrame(core::DirtyFlag dirty) noexcept {
    if (dirty != core::DirtyFlag::none) {
        sampleIndex_ = 0;
        needsClear_ = true;
        log::debug("BDPT accumulation reset: dirty, sample 0");
        return true;
    }
    if (sampleIndex_ < std::numeric_limits<std::uint32_t>::max()) {
        ++sampleIndex_;
    }
    needsClear_ = false;
    log::debug("BDPT accumulation continue: sample ", sampleIndex_);
    return false;
}

bool BdptAccumulation::syncScene(core::Scene& scene) {
    ensurePersistent();
    const core::DirtyFlag dirty = scene.dirtyFlags();
    if (dirty != core::DirtyFlag::none) {
        sampleIndex_ = 0;
        needsClear_ = true;
        scene.invalidateAccumulation();
        log::debug("BDPT accumulation reset with scene: sample 0");
        return true;
    }
    scene.advanceAccumulation();
    const auto samples = scene.accumulatedSamples();
    sampleIndex_ = samples > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                                       : static_cast<std::uint32_t>(samples);
    needsClear_ = false;
    log::debug("BDPT accumulation synced with scene: sample ", sampleIndex_);
    return false;
}

} // namespace dayo::graphics
