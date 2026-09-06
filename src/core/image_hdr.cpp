#include "core/image_hdr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dayo::core {
namespace {

std::size_t checkedMul(std::size_t lhs, std::size_t rhs, const char* message) {
    if (rhs != 0 && lhs > std::numeric_limits<std::size_t>::max() / rhs)
        throw std::overflow_error(message);
    return lhs * rhs;
}

std::size_t imageByteCount(const ImageData& image) {
    if (image.width == 0 || image.height == 0 || image.channels == 0)
        throw std::invalid_argument("image has an empty extent or channel count");
    const auto pixels = checkedMul(static_cast<std::size_t>(image.width), static_cast<std::size_t>(image.height),
                                   "image pixel count overflow");
    const auto samples = checkedMul(pixels, static_cast<std::size_t>(image.channels), "image sample count overflow");
    const auto expected = checkedMul(samples, pixelByteSize(image.type, 1), "image byte count overflow");
    if (image.bytes.size() < expected)
        throw std::invalid_argument("truncated image data");
    return expected;
}

float srgbToLinear(float value) noexcept {
    if (value <= 0.04045F)
        return value / 12.92F;
    return std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linearToSrgb(float value) noexcept {
    if (value <= 0.0031308F)
        return value * 12.92F;
    return 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

float readSample(const ImageData& image, std::size_t index) {
    if (image.type == PixelType::unorm8)
        return static_cast<float>(image.bytes[index]) / 255.0F;
    if (image.type == PixelType::half16) {
        std::uint16_t bits{};
        std::memcpy(&bits, image.bytes.data() + index * sizeof(bits), sizeof(bits));
        return halfToFloat(bits);
    }
    float value{};
    std::memcpy(&value, image.bytes.data() + index * sizeof(value), sizeof(value));
    return value;
}

void writeSample(ImageData& image, std::size_t index, float value) {
    if (image.type == PixelType::unorm8) {
        if (std::isnan(value))
            value = 0.0F;
        else
            value = std::clamp(value, 0.0F, 1.0F);
        image.bytes[index] = static_cast<std::uint8_t>(std::lround(value * 255.0F));
        return;
    }
    if (image.type == PixelType::half16) {
        const auto bits = floatToHalf(value);
        std::memcpy(image.bytes.data() + index * sizeof(bits), &bits, sizeof(bits));
        return;
    }
    std::memcpy(image.bytes.data() + index * sizeof(value), &value, sizeof(value));
}

} // namespace

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
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<int>((bits >> 23U) & 0xFFU);
    const auto mantissa = bits & 0x7FFFFFU;
    if (exponent == 0xFF) {
        if (mantissa == 0)
            return static_cast<std::uint16_t>(sign | 0x7C00U);
        // Preserve NaN-ness and at least one payload bit after narrowing.
        const auto payload = static_cast<std::uint16_t>(mantissa >> 13U);
        return static_cast<std::uint16_t>(sign | 0x7C00U | (payload == 0 ? 1U : payload));
    }
    const int halfExponent = exponent - 127 + 15;
    if (halfExponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00U);

    if (halfExponent <= 0) {
        if (halfExponent < -10)
            return sign;
        const std::uint32_t significand = mantissa | 0x00800000U;
        const int shift = 14 - halfExponent;
        std::uint32_t rounded = significand >> shift;
        const std::uint32_t remainder = significand & ((1U << shift) - 1U);
        const std::uint32_t halfway = 1U << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (rounded & 1U) != 0U))
            ++rounded;
        return static_cast<std::uint16_t>(sign | rounded);
    }

    std::uint32_t roundedMantissa = mantissa >> 13U;
    const std::uint32_t remainder = mantissa & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (roundedMantissa & 1U) != 0U))
        ++roundedMantissa;
    int finalExponent = halfExponent;
    if (roundedMantissa == 0x400U) {
        roundedMantissa = 0;
        ++finalExponent;
    }
    if (finalExponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint16_t>(finalExponent) << 10U) |
                                      static_cast<std::uint16_t>(roundedMantissa));
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
            std::uint32_t normalized = mantissa;
            int subExponent = -14;
            while ((normalized & 0x400U) == 0U) {
                normalized <<= 1U;
                --subExponent;
            }
            bits = sign | (static_cast<std::uint32_t>(subExponent + 127) << 23U) | ((normalized & 0x3FFU) << 13U);
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
    const std::size_t pixels = checkedMul(static_cast<std::size_t>(image.width), static_cast<std::size_t>(image.height),
                                          "RGBA8 pixel count overflow");
    const std::size_t samples = checkedMul(pixels, 4U, "RGBA8 sample count overflow");
    if (image.pixels.size() < samples)
        throw std::invalid_argument("truncated RGBA8 image");
    result.bytes.resize(checkedMul(samples, sizeof(std::uint16_t), "half image byte count overflow"));
    for (std::size_t index = 0; index < pixels; ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const float value = static_cast<float>(image.pixels[index * 4U + channel]) / 255.0F;
            const auto bits = floatToHalf(value);
            std::memcpy(result.bytes.data() + (index * 4U + channel) * sizeof(bits), &bits, sizeof(bits));
        }
    }
    return result;
}

ImageRgba8 halfToRgba8(const ImageData& image) {
    if (image.type != PixelType::half16 || image.channels != 4)
        throw std::invalid_argument("expected RGBA half image");
    imageByteCount(image);
    const std::size_t pixels = checkedMul(static_cast<std::size_t>(image.width), static_cast<std::size_t>(image.height),
                                          "image pixel count overflow");
    ImageRgba8 result{image.width, image.height, std::vector<std::uint8_t>(pixels * 4U)};
    for (std::size_t index = 0; index < pixels; ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            std::uint16_t bits{};
            std::memcpy(&bits, image.bytes.data() + (index * 4U + channel) * sizeof(bits), sizeof(bits));
            const float value = halfToFloat(bits);
            result.pixels[index * 4U + channel] =
                static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
        }
    }
    return result;
}

ImageData convertImage(const ImageData& image, PixelType target, ColorSpace targetSpace) {
    imageByteCount(image);
    ImageData result;
    result.width = image.width;
    result.height = image.height;
    result.channels = image.channels;
    result.type = target;
    result.space = targetSpace;
    const auto pixels = checkedMul(static_cast<std::size_t>(image.width), static_cast<std::size_t>(image.height),
                                   "image pixel count overflow");
    const auto samples = checkedMul(pixels, static_cast<std::size_t>(image.channels), "image sample count overflow");
    result.bytes.resize(checkedMul(samples, pixelByteSize(target, 1), "image byte count overflow"));
    for (std::size_t sample = 0; sample < samples; ++sample) {
        float value = readSample(image, sample);
        const auto channel = sample % image.channels;
        if (channel < 3U && image.space != targetSpace) {
            if (image.space == ColorSpace::srgb && targetSpace == ColorSpace::linear)
                value = srgbToLinear(value);
            else if (image.space == ColorSpace::linear && targetSpace == ColorSpace::srgb)
                value = linearToSrgb(value);
        }
        writeSample(result, sample, value);
    }
    return result;
}

} // namespace dayo::core
