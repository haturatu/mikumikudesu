#include "core/image_hdr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace dayo::core {

std::size_t pixelByteSize(PixelType type, std::uint32_t channels) noexcept {
    std::size_t perChannel = 1;
    switch (type) {
    case PixelType::unorm8:
        perChannel = 1;
        break;
    case PixelType::half16:
        perChannel = 2;
        break;
    case PixelType::float32:
        perChannel = 4;
        break;
    }
    return perChannel * channels;
}

std::uint16_t floatToHalf(float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 112;
    const auto mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0) {
        // Subnormal / zero skeleton: flush tiny values toward zero.
        return sign;
    }
    if (exponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7BFFU);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint16_t>(exponent) << 10U) |
                                      static_cast<std::uint16_t>(mantissa >> 13U));
}

float halfToFloat(std::uint16_t value) noexcept {
    const auto sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
    const auto exponent = static_cast<std::uint32_t>((value >> 10U) & 0x1FU);
    const auto mantissa = static_cast<std::uint32_t>(value & 0x3FFU);
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Normalize subnormal skeleton input.
            float result = static_cast<float>(mantissa) / 1024.0F;
            result /= 16384.0F;
            return (sign != 0U ? -result : result);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

ImageData rgba8ToHalf(const ImageRgba8& image, ColorSpace space) {
    if (image.width == 0 || image.height == 0 || image.pixels.empty())
        throw std::invalid_argument("empty LDR image");
    ImageData result;
    result.width = image.width;
    result.height = image.height;
    result.channels = 4;
    result.type = PixelType::half16;
    result.space = space;
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    result.bytes.resize(pixels * 4U * sizeof(std::uint16_t));
    auto* destination = reinterpret_cast<std::uint16_t*>(result.bytes.data());
    for (std::size_t index = 0; index < pixels; ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const float value = static_cast<float>(image.pixels[index * 4U + channel]) / 255.0F;
            destination[index * 4U + channel] = floatToHalf(value);
        }
    }
    return result;
}

ImageRgba8 halfToRgba8(const ImageData& image) {
    if (image.type != PixelType::half16 || image.channels != 4)
        throw std::invalid_argument("expected RGBA half image");
    const std::size_t pixels = image.pixelCount();
    if (image.bytes.size() < pixels * 4U * sizeof(std::uint16_t))
        throw std::invalid_argument("truncated half image");
    ImageRgba8 result{image.width, image.height, std::vector<std::uint8_t>(pixels * 4U)};
    const auto* source = reinterpret_cast<const std::uint16_t*>(image.bytes.data());
    for (std::size_t index = 0; index < pixels; ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const float value = halfToFloat(source[index * 4U + channel]);
            result.pixels[index * 4U + channel] =
                static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        }
    }
    return result;
}

ImageData convertImage(const ImageData& image, PixelType target, ColorSpace targetSpace) {
    if (image.empty())
        throw std::invalid_argument("empty image");
    if (image.type == target && image.space == targetSpace)
        return image;
    // Skeleton path: half <-> unorm8 round-trips through float helpers.
    // float32 I/O lands here additively without disturbing the LDR path.
    if (image.type == PixelType::unorm8 && target == PixelType::half16) {
        ImageRgba8 ldr{image.width, image.height, {}};
        const std::size_t pixels = image.pixelCount() * image.channels;
        ldr.pixels.assign(image.bytes.begin(), image.bytes.begin() + static_cast<std::ptrdiff_t>(pixels));
        if (image.channels != 4) {
            ImageRgba8 expanded{image.width, image.height, std::vector<std::uint8_t>(image.pixelCount() * 4U, 255)};
            for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel)
                for (std::uint32_t channel = 0; channel < image.channels && channel < 4; ++channel)
                    expanded.pixels[pixel * 4U + channel] = ldr.pixels[pixel * image.channels + channel];
            return rgba8ToHalf(expanded, targetSpace);
        }
        return rgba8ToHalf(ldr, targetSpace);
    }
    ImageData copy = image;
    copy.type = target;
    copy.space = targetSpace;
    return copy;
}

} // namespace dayo::core
