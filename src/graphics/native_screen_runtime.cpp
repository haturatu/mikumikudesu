#include "graphics/native_screen_runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

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

[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept {
    // The source is normalized RGBA8, so finite values in [0, 1] are the
    // common case. Keep the conversion complete nevertheless because this
    // helper is also used for test images with arbitrary channel bytes.
    std::uint32_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<int>((bits >> 23U) & 0xffU) - 127 + 15;
    const auto mantissa = bits & 0x7fffffU;
    if (exponent <= 0) {
        if (exponent < -10)
            return sign;
        const auto shifted = (mantissa | 0x800000U) >> static_cast<unsigned>(1 - exponent);
        return static_cast<std::uint16_t>(sign | ((shifted + 0x1000U) >> 13U));
    }
    if (exponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7c00U | (mantissa != 0 ? 0x0200U : 0U));
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10U) |
                                      ((mantissa + 0x1000U) >> 13U));
}

[[nodiscard]] std::vector<std::uint8_t> makeRgba16Source(const core::ImageRgba8& image, Extent3D target,
                                                         NativeScreenCrop crop) {
    if (image.width == 0 || image.height == 0)
        throw std::invalid_argument("native ScreenBMP source image is empty");
    const auto expected = static_cast<std::uint64_t>(image.width) * image.height * 4U;
    if (expected != image.pixels.size())
        throw std::invalid_argument("native ScreenBMP source image has an invalid RGBA8 byte count");
    if (target.width == 0 || target.height == 0 || target.depth != 1)
        throw std::invalid_argument("native ScreenBMP target extent is invalid");

    double left = 0.0;
    double top = 0.0;
    double sourceWidth = static_cast<double>(image.width);
    double sourceHeight = static_cast<double>(image.height);
    if (crop == NativeScreenCrop::crop4x3) {
        const auto sourceAspect = sourceWidth / sourceHeight;
        constexpr double targetAspect = 4.0 / 3.0;
        if (sourceAspect > targetAspect) {
            sourceWidth = sourceHeight * targetAspect;
            left = (static_cast<double>(image.width) - sourceWidth) * 0.5;
        } else if (sourceAspect < targetAspect) {
            sourceHeight = sourceWidth / targetAspect;
            top = (static_cast<double>(image.height) - sourceHeight) * 0.5;
        }
    }

    const auto pixelCount = static_cast<std::uint64_t>(target.width) * target.height;
    if (pixelCount > std::numeric_limits<std::size_t>::max() / 8U)
        throw std::overflow_error("native ScreenBMP target image is too large");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(pixelCount) * 8U);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        const auto sourceY =
            std::min(image.height - 1U,
                     static_cast<std::uint32_t>((static_cast<double>(y) + 0.5) * sourceHeight / target.height + top));
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const auto sourceX = std::min(
                image.width - 1U,
                static_cast<std::uint32_t>((static_cast<double>(x) + 0.5) * sourceWidth / target.width + left));
            const auto sourceOffset = (static_cast<std::size_t>(sourceY) * image.width + sourceX) * 4U;
            const auto destinationOffset = (static_cast<std::size_t>(y) * target.width + x) * 8U;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const auto normalized = static_cast<float>(image.pixels[sourceOffset + channel]) / 255.0F;
                const auto half = floatToHalf(normalized);
                result[destinationOffset + channel * 2U] = static_cast<std::uint8_t>(half & 0xffU);
                result[destinationOffset + channel * 2U + 1U] = static_cast<std::uint8_t>(half >> 8U);
            }
        }
    }
    return result;
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
        screenBmp_ = device.createTextureEx(description);
        screenTexture_ = device.createTextureEx(description);
        previousFrame_ = device.createTextureEx(description);
        if (!screenBmp_.valid() || !screenTexture_.valid() || !previousFrame_.valid())
            throw std::runtime_error("native screen runtime allocated an invalid texture");
        // The first frame has no completed output. Upstream screen.bmp uses a
        // white fallback until the first frame has been published.
        constexpr std::array white{1.0F, 1.0F, 1.0F, 1.0F};
        device.clearTextureEx(screenBmp_, white);
        device.clearTextureEx(screenTexture_, white);
        device.clearTextureEx(previousFrame_, white);
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
    return screenBmp_.valid() && screenTexture_.valid() && previousFrame_.valid();
}

bool NativeScreenRuntime::matchesExtent(Extent3D extent) const noexcept {
    return extent_.width == extent.width && extent_.height == extent.height && extent_.depth == extent.depth;
}

handles::TextureHandle NativeScreenRuntime::screenBmp() const noexcept {
    return screenBmp_;
}

handles::TextureHandle NativeScreenRuntime::screenTexture() const noexcept {
    return screenTexture_;
}

handles::TextureHandle NativeScreenRuntime::previousFrame() const noexcept {
    return previousFrame_;
}

void NativeScreenRuntime::prepareFrame(CommandList& commands, NativeScreenSource source, bool enabled,
                                       NativeScreenCrop crop) {
    if (!ready())
        throw std::logic_error("native screen runtime is not ready");
    commands.transferBarrierEx();
    commands.copyTextureEx(previousFrame_, screenTexture_);
    if (!enabled || source == NativeScreenSource::white) {
        commands.clearTextureEx(screenBmp_, {1.0F, 1.0F, 1.0F, 1.0F});
    } else if (source == NativeScreenSource::previousFrame) {
        if (crop == NativeScreenCrop::none) {
            commands.copyTextureEx(previousFrame_, screenBmp_);
        } else {
            const auto width = extent_.width;
            const auto height = extent_.height;
            std::uint32_t croppedWidth = width;
            std::uint32_t croppedHeight = height;
            if (static_cast<std::uint64_t>(width) * 3U > static_cast<std::uint64_t>(height) * 4U)
                croppedWidth = static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * 4U / 3U);
            else
                croppedHeight = static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) * 3U / 4U);
            croppedWidth = std::max(1U, croppedWidth);
            croppedHeight = std::max(1U, croppedHeight);
            const auto left = (width - croppedWidth) / 2U;
            const auto top = (height - croppedHeight) / 2U;
            commands.blitTextureEx(previousFrame_, screenBmp_, {left, top, left + croppedWidth, top + croppedHeight});
        }
    }
}

bool NativeScreenRuntime::uploadScreenBmp(const core::ImageRgba8& image, NativeScreenCrop crop, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready()) {
        setError(error, "native ScreenBMP runtime is not ready");
        return false;
    }
    try {
        const auto bytes = makeRgba16Source(image, extent_, crop);
        device_->uploadTextureEx(screenBmp_, bytes, 0, 0);
    } catch (const std::exception& exception) {
        setError(error, std::string("native ScreenBMP upload failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native ScreenBMP upload failed");
        return false;
    }
    return true;
}

void NativeScreenRuntime::publishFrame(CommandList& commands, handles::TextureHandle currentFinal) {
    if (!ready() || !currentFinal.valid())
        throw std::logic_error("native screen history is not ready");
    commands.transferBarrierEx();
    if (currentFinal != screenTexture_)
        commands.copyTextureEx(currentFinal, screenTexture_);
    if (currentFinal != previousFrame_)
        commands.copyTextureEx(currentFinal, previousFrame_);
}

void NativeScreenRuntime::rotatePreviousFrame(CommandList& commands, handles::TextureHandle currentFinal) {
    if (!ready() || !currentFinal.valid())
        throw std::logic_error("native screen history is not ready");
    commands.transferBarrierEx();
    if (currentFinal != previousFrame_)
        commands.copyTextureEx(currentFinal, previousFrame_);
}

void NativeScreenRuntime::bindScreenSemantics(NativeSceneResourceBindings& bindings) const noexcept {
    if (!ready())
        return;
    bindings.screenBmp = screenBmp();
    bindings.screenTexture = screenTexture();
    bindings.hostResourceMask |=
        dayoSemanticBit(DayoSemantic::ScreenBMP) | dayoSemanticBit(DayoSemantic::ScreenTexture);
}

void NativeScreenRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (const auto texture : {screenBmp_, screenTexture_, previousFrame_}) {
            if (!texture.valid())
                continue;
            try {
                device->destroyTextureEx(texture);
            } catch (...) {
            }
        }
    }
    device_ = nullptr;
    extent_ = {};
    screenBmp_ = {};
    screenTexture_ = {};
    previousFrame_ = {};
}

} // namespace dayo::graphics
