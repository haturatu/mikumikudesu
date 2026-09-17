#pragma once

#include "fx/fx_frame.hpp"
#include "graphics/device.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <string>

namespace dayo::graphics {

// Native view constants mirror hlsl/cb.hlsli. Every member group occupies one
// 16-byte constant-buffer lane; this avoids relying on compiler-specific
// packing rules when the bytes are consumed by HLSL.
struct alignas(16) NativeViewConstants {
    std::array<float, 16> viewMatrix{};
    std::array<float, 16> projectionMatrix{};
    std::array<std::int32_t, 2> cameraFlags{};
    std::array<std::uint32_t, 2> modelCounts{};
    std::array<float, 4> frameTimes{};
    std::array<float, 2> realTimes{};
    std::array<std::int32_t, 2> mouseButtons{};
    std::array<float, 2> mousePosition{};
    std::int32_t playing{};
    std::int32_t constantPadding{};
    std::array<std::uint32_t, 4> output{};
    std::array<float, 3> lightColor{};
    std::int32_t selfShadowMode{};
    std::array<float, 3> lightDirection{};
    float selfShadowDistance{};
    float sceneRadius{};
    std::int32_t screenBmpMode{};
    std::int32_t backgroundMode{};
    std::int32_t backgroundTransparent{};
    std::int32_t materialHighlight{};
    std::int32_t denoiserEnabled{};
    std::int32_t onStart{};
    std::int32_t onLoadSkybox{};
    std::int32_t onResize{};
    std::int32_t onLoad{};
    std::array<std::int32_t, 2> eventPadding{};
};
static_assert(sizeof(NativeViewConstants) == 288);
static_assert(alignof(NativeViewConstants) == 16);

// CBuff1 is used by raster and deform passes to select the current model and
// operation order. It lives in descriptor set 1 at uniform register b0.
struct alignas(16) NativeScenePassConstants {
    std::uint32_t modelIndex{};
    std::uint32_t rasterizeOrder{};
    std::uint32_t deformIndex{};
    std::uint32_t deformOrder{};
};
static_assert(sizeof(NativeScenePassConstants) == 16);
static_assert(alignof(NativeScenePassConstants) == 16);

// Supplies the fields that are derivable from the backend-neutral FX frame.
// Matrices and event/system flags stay explicit in NativeViewConstants so the
// application can replace the identity defaults when it owns those values.
[[nodiscard]] NativeViewConstants makeNativeViewConstants(const fx::FxFrameContext& context, std::uint32_t modelCount,
                                                          std::uint32_t totalMaterialCount) noexcept;

// Owns the persistent uniform buffers for ViewCB and CBuff1. Controller
// constants are deliberately separate: their generated layout is effect
// specific and will be bound by the FX controller runtime.
class NativeFrameConstantsRuntime {
  public:
    NativeFrameConstantsRuntime() = default;
    ~NativeFrameConstantsRuntime();

    NativeFrameConstantsRuntime(const NativeFrameConstantsRuntime&) = delete;
    NativeFrameConstantsRuntime& operator=(const NativeFrameConstantsRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, std::string* error = nullptr);
    [[nodiscard]] bool syncView(Device& device, const NativeViewConstants& view, std::string* error = nullptr);
    [[nodiscard]] bool sync(Device& device, const NativeViewConstants& view, const NativeScenePassConstants& pass,
                            std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && std::all_of(viewBuffers_.begin(), viewBuffers_.end(),
                                                 [](const auto buffer) { return buffer.valid(); }) &&
               std::all_of(passBuffers_.begin(), passBuffers_.end(),
                           [](const auto buffer) { return buffer.valid(); });
    }
    [[nodiscard]] handles::BufferHandle viewBuffer() const noexcept {
        return device_ == nullptr ? handles::BufferHandle{}
                                   : viewBuffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
    }
    [[nodiscard]] handles::BufferHandle passBuffer() const noexcept {
        return device_ == nullptr ? handles::BufferHandle{}
                                   : passBuffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
    }

  private:
    Device* device_{};
    std::array<handles::BufferHandle, kNativeFramesInFlight> viewBuffers_{};
    std::array<handles::BufferHandle, kNativeFramesInFlight> passBuffers_{};
};

} // namespace dayo::graphics
