#include "graphics/native_frame_constants.hpp"

#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] std::array<float, 16> identityMatrix() noexcept {
    return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

[[nodiscard]] std::uint32_t narrowSample(std::uint64_t sample) noexcept {
    return sample > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                               : static_cast<std::uint32_t>(sample);
}

} // namespace

NativeViewConstants makeNativeViewConstants(const fx::FxFrameContext& context, std::uint32_t modelCount,
                                            std::uint32_t totalMaterialCount) noexcept {
    NativeViewConstants result;
    result.viewMatrix = identityMatrix();
    result.projectionMatrix = identityMatrix();
    result.cameraFlags = {context.camera.perspective ? 1 : 0, 0};
    result.modelCounts = {modelCount, totalMaterialCount};
    const auto frameTime = context.frame / 30.0F;
    result.frameTimes = {frameTime, 0.0F, frameTime, 0.0F};
    result.output = {context.renderWidth, context.renderHeight, narrowSample(context.sample), 1};
    result.lightColor = context.lighting.color;
    result.lightDirection = context.lighting.direction;
    return result;
}

NativeFrameConstantsRuntime::~NativeFrameConstantsRuntime() {
    reset();
}

bool NativeFrameConstantsRuntime::initialize(Device& device, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        const BufferResourceDesc description{
            .size = sizeof(NativeViewConstants),
            .usage = ResourceUsage::uniformRead | ResourceUsage::transferDst,
            .cpuVisible = false,
            .lifetime = ResourceLifetime::persistent,
        };
        viewBuffer_ = device.createBufferEx(description);
        if (!viewBuffer_.valid())
            throw std::runtime_error("native ViewCB buffer is invalid");
        passBuffer_ = device.createBufferEx({
            .size = sizeof(NativeScenePassConstants),
            .usage = ResourceUsage::uniformRead | ResourceUsage::transferDst,
            .cpuVisible = false,
            .lifetime = ResourceLifetime::persistent,
        });
        if (!passBuffer_.valid())
            throw std::runtime_error("native CBuff1 buffer is invalid");
    } catch (const std::exception& exception) {
        setError(error, std::string("native frame constants initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native frame constants initialization failed");
        reset();
        return false;
    }
    return true;
}

bool NativeFrameConstantsRuntime::sync(Device& device, const NativeViewConstants& view,
                                       const NativeScenePassConstants& pass, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr && !initialize(device, error))
        return false;
    if (device_ != &device) {
        setError(error, "native frame constants belong to a different device");
        return false;
    }
    try {
        device_->uploadBufferEx(viewBuffer_, std::as_bytes(std::span<const NativeViewConstants>(&view, 1)), 0);
        device_->uploadBufferEx(passBuffer_, std::as_bytes(std::span<const NativeScenePassConstants>(&pass, 1)), 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("native frame constants upload failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native frame constants upload failed");
        return false;
    }
    return true;
}

void NativeFrameConstantsRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        if (viewBuffer_.valid()) {
            try {
                device->destroyBufferEx(viewBuffer_);
            } catch (...) {
            }
        }
        if (passBuffer_.valid()) {
            try {
                device->destroyBufferEx(passBuffer_);
            } catch (...) {
            }
        }
    }
    device_ = nullptr;
    viewBuffer_ = {};
    passBuffer_ = {};
}

} // namespace dayo::graphics
