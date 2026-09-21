#include "graphics/native_screen_runtime.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

TextureResourceDesc screenTextureDesc(Extent3D extent) {
    return {.dimension = TextureDimension::d2,
            .extent = extent,
            .format = PixelFormat::rgba16Float,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = ResourceUsage::sampledRead | ResourceUsage::storageReadWrite | ResourceUsage::colorAttachment |
                     ResourceUsage::transferSrc | ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent};
}

} // namespace

NativeScreenRuntime::~NativeScreenRuntime() {
    reset();
}

bool NativeScreenRuntime::initialize(Device& device, Extent3D extent, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    if (extent.width == 0 || extent.height == 0 || extent.depth != 1) {
        setError(error, "native screen runtime requires a non-empty 2D extent");
        return false;
    }
    device_ = &device;
    extent_ = extent;
    try {
        const auto description = screenTextureDesc(extent);
        for (auto& slot : slots_) {
            slot.screenBmp = device.createTextureEx(description);
            slot.screenTexture = device.createTextureEx(description);
            slot.previousFrame = device.createTextureEx(description);
            if (!slot.screenBmp.valid() || !slot.screenTexture.valid() || !slot.previousFrame.valid())
                throw std::runtime_error("native screen runtime allocated an invalid texture");
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("native screen runtime initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native screen runtime initialization failed");
        reset();
        return false;
    }
    return true;
}

bool NativeScreenRuntime::ready() const noexcept {
    if (device_ == nullptr)
        return false;
    for (const auto& slot : slots_) {
        if (!slot.screenBmp.valid() || !slot.screenTexture.valid() || !slot.previousFrame.valid())
            return false;
    }
    return true;
}

const NativeScreenRuntime::Slot& NativeScreenRuntime::currentSlot() const noexcept {
    return slots_[device_ == nullptr ? 0 : device_->currentFrameSlot() % kNativeFramesInFlight];
}

handles::TextureHandle NativeScreenRuntime::screenBmp() const noexcept {
    return currentSlot().screenBmp;
}

handles::TextureHandle NativeScreenRuntime::screenTexture() const noexcept {
    return currentSlot().screenTexture;
}

handles::TextureHandle NativeScreenRuntime::previousFrame() const noexcept {
    return currentSlot().previousFrame;
}

void NativeScreenRuntime::rotatePreviousFrame(CommandList& commands, handles::TextureHandle currentFinal) {
    if (!ready() || !currentFinal.valid())
        throw std::logic_error("native screen history is not ready");
    commands.transferBarrierEx();
    commands.copyTextureEx(currentFinal, previousFrame());
}

void NativeScreenRuntime::bindScreenSemantics(NativeSceneResourceBindings& bindings) const noexcept {
    if (!ready())
        return;
    bindings.screenBmp = screenBmp();
    bindings.screenTexture = screenTexture();
    bindings.hostResourceMask |= dayoSemanticBit(DayoSemantic::ScreenBMP) |
                                 dayoSemanticBit(DayoSemantic::ScreenTexture);
}

void NativeScreenRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (const auto& slot : slots_) {
            for (const auto texture : {slot.screenBmp, slot.screenTexture, slot.previousFrame}) {
                if (!texture.valid())
                    continue;
                try {
                    device->destroyTextureEx(texture);
                } catch (...) {
                }
            }
        }
    }
    device_ = nullptr;
    extent_ = {};
    slots_ = {};
}

} // namespace dayo::graphics
