#include "graphics/native_frame_constants.hpp"

#include <array>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] std::uint32_t narrowSample(std::uint64_t sample) noexcept {
    return sample > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                              : static_cast<std::uint32_t>(sample);
}

} // namespace

NativeViewConstants makeNativeViewConstants(const fx::FxFrameContext& context,
                                            std::uint32_t totalMaterialCount) noexcept {
    NativeViewConstants result;
    result.viewMatrix = context.camera.view;
    result.projectionMatrix = context.camera.projection;
    result.cameraFlags = {context.camera.perspective ? 1 : 0, 0};
    result.modelCounts = {context.modelCount, totalMaterialCount};
    const auto frameTime = context.frame / 30.0F;
    result.frameTimes = {frameTime, 0.0F, frameTime, 0.0F};
    result.output = {context.renderWidth, context.renderHeight, narrowSample(context.sample), 1};
    result.playing = context.host.playing ? 1 : 0;
    result.lightColor = context.lighting.color;
    result.lightDirection = context.lighting.direction;
    result.selfShadowMode = context.host.selfShadowMode;
    result.selfShadowDistance = context.host.selfShadowDistance;
    result.screenBmpMode = context.host.screenBmpMode;
    result.backgroundMode = context.host.backgroundMode;
    result.backgroundTransparent = context.host.backgroundTransparent ? 1 : 0;
    result.denoiserEnabled = context.host.denoiserEnabled ? 1 : 0;
    result.onStart = context.host.onStart ? 1 : 0;
    result.onLoadSkybox = context.host.onLoadSkybox ? 1 : 0;
    result.onResize = context.host.onResize ? 1 : 0;
    result.onLoad = context.host.onLoad ? 1 : 0;
    return result;
}

void populateNativeViewExpressionSymbols(fx::FxFrameContext& context, const NativeViewConstants& view) {
    const auto setInteger = [&context](std::string_view name, std::int64_t value) {
        context.expressionSymbols.insert_or_assign(std::string(name), core::fx::FxScalar{value});
    };
    const auto setFloat = [&context](std::string_view name, float value) {
        context.expressionSymbols.insert_or_assign(std::string(name), core::fx::FxScalar{static_cast<double>(value)});
    };

    setInteger("Perspective", view.cameraFlags[0]);
    setInteger("CameraInterpolated", view.cameraFlags[1]);
    setInteger("ModelCount", view.modelCounts[0]);
    setInteger("TotalMaterialCount", view.modelCounts[1]);
    context.time = view.frameTimes[0];
    setFloat("DTime", view.frameTimes[1]);
    setFloat("FrameTime", view.frameTimes[2]);
    setFloat("DFrameTime", view.frameTimes[3]);
    setFloat("RealTime", view.realTimes[0]);
    setFloat("DRealTime", view.realTimes[1]);
    setInteger("MouseDown", view.mouseButtons[0]);
    setInteger("MouseClicked", view.mouseButtons[1]);
    setFloat("MousePos.x", view.mousePosition[0]);
    setFloat("MousePos.y", view.mousePosition[1]);
    setInteger("Playing", view.playing);
    setInteger("Resolution.x", view.output[0]);
    setInteger("Resolution.y", view.output[1]);
    setInteger("iSample", view.output[2]);
    setInteger("SamplesPerFrame", view.output[3]);
    setFloat("LightColor.x", view.lightColor[0]);
    setFloat("LightColor.y", view.lightColor[1]);
    setFloat("LightColor.z", view.lightColor[2]);
    setInteger("SelfShadowMode", view.selfShadowMode);
    setFloat("LightDirection.x", view.lightDirection[0]);
    setFloat("LightDirection.y", view.lightDirection[1]);
    setFloat("LightDirection.z", view.lightDirection[2]);
    setFloat("SelfShadowDistance", view.selfShadowDistance);
    setFloat("SceneRadius", view.sceneRadius);
    setInteger("ScreenBMPMode", view.screenBmpMode);
    setInteger("BackgroundMode", view.backgroundMode);
    setInteger("BackgroundTransparent", view.backgroundTransparent);
    setInteger("MaterialHighLight", view.materialHighlight);
    setInteger("DenoiserEnabled", view.denoiserEnabled);
    setInteger("OnStart", view.onStart);
    setInteger("OnLoadSkybox", view.onLoadSkybox);
    setInteger("OnResize", view.onResize);
    setInteger("OnLoad", view.onLoad);
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
