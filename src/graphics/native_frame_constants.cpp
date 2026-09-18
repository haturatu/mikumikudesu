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
    return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
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
        for (auto& buffer : viewBuffers_) {
            buffer = device.createBufferEx({
                .size = sizeof(NativeViewConstants),
                .usage = ResourceUsage::uniformRead | ResourceUsage::hostRead,
                .cpuVisible = true,
                .lifetime = ResourceLifetime::persistent,
            });
            if (!buffer.valid())
                throw std::runtime_error("native ViewCB buffer is invalid");
        }
        for (auto& buffer : passBuffers_) {
            buffer = device.createBufferEx({
                .size = sizeof(NativeScenePassConstants),
                .usage = ResourceUsage::uniformRead | ResourceUsage::hostRead,
                .cpuVisible = true,
                .lifetime = ResourceLifetime::persistent,
            });
            if (!buffer.valid())
                throw std::runtime_error("native CBuff1 buffer is invalid");
        }
        const NativeViewConstants initialView{};
        const NativeScenePassConstants initialPass{};
        for (const auto buffer : viewBuffers_)
            device.uploadBufferEx(buffer, std::as_bytes(std::span<const NativeViewConstants>(&initialView, 1)), 0);
        for (const auto buffer : passBuffers_)
            device.uploadBufferEx(buffer, std::as_bytes(std::span<const NativeScenePassConstants>(&initialPass, 1)), 0);
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

bool NativeFrameConstantsRuntime::syncView(Device& device, const NativeViewConstants& view, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr && !initialize(device, error))
        return false;
    if (device_ != &device) {
        setError(error, "native frame constants belong to a different device");
        return false;
    }
    try {
        const auto buffer = viewBuffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
        device_->uploadBufferEx(buffer, std::as_bytes(std::span<const NativeViewConstants>(&view, 1)), 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("native ViewCB upload failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native ViewCB upload failed");
        return false;
    }
    return true;
}

bool NativeFrameConstantsRuntime::sync(Device& device, const NativeViewConstants& view,
                                       const NativeScenePassConstants& pass, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!syncView(device, view, error))
        return false;
    try {
        const auto buffer = passBuffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
        device_->uploadBufferEx(buffer, std::as_bytes(std::span<const NativeScenePassConstants>(&pass, 1)), 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("native CBuff1 upload failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native CBuff1 upload failed");
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
        for (const auto buffer : viewBuffers_) {
            if (buffer.valid()) {
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
        }
        for (const auto buffer : passBuffers_) {
            if (buffer.valid()) {
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
        }
    }
    device_ = nullptr;
    viewBuffers_.fill({});
    passBuffers_.fill({});
}

} // namespace dayo::graphics
