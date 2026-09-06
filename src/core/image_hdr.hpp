#pragma once

#include "core/image.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dayo::core {

enum class PixelType : std::uint8_t { unorm8 = 0, half16 = 1, float32 = 2 };
enum class ColorSpace : std::uint8_t { linear = 0, srgb = 1 };

// HDR-capable image container. Existing ImageRgba8 stays the LDR fast path;
// ImageData carries the pixel type + color space explicitly for EXR/HALF I/O.
struct ImageData {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t channels{4};
    PixelType type{PixelType::unorm8};
    ColorSpace space{ColorSpace::srgb};
    std::vector<std::uint8_t> bytes;
    [[nodiscard]] std::size_t pixelCount() const noexcept {
        return static_cast<std::size_t>(width) * height;
    }
    [[nodiscard]] std::size_t byteSize() const noexcept {
        return bytes.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return bytes.empty() || width == 0 || height == 0;
    }
};

[[nodiscard]] std::size_t pixelByteSize(PixelType type, std::uint32_t channels) noexcept;
[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept;
[[nodiscard]] float halfToFloat(std::uint16_t value) noexcept;

// RGBA16F <-> HALF skeleton: LDR bytes convert through linear-half storage.
[[nodiscard]] ImageData rgba8ToHalf(const ImageRgba8& image, ColorSpace space = ColorSpace::linear);
[[nodiscard]] ImageRgba8 halfToRgba8(const ImageData& image);
[[nodiscard]] ImageData convertImage(const ImageData& image, PixelType target, ColorSpace targetSpace);

} // namespace dayo::core
