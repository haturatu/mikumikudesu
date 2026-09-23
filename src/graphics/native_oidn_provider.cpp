#include "graphics/native_oidn_provider.hpp"
#include "graphics/native_scene_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {

void NativeOidnProvider::setError(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

float NativeOidnProvider::halfToFloat(std::uint16_t value) noexcept {
    const auto sign = static_cast<std::uint32_t>(value >> 15U);
    const auto exponent = static_cast<std::uint32_t>((value >> 10U) & 0x1fU);
    const auto mantissa = static_cast<std::uint32_t>(value & 0x3ffU);
    std::uint32_t bits{};
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign << 31U;
        } else {
            auto normalized = mantissa;
            std::uint32_t exponentValue = 127U - 14U;
            while ((normalized & 0x400U) == 0) {
                normalized <<= 1U;
                --exponentValue;
            }
            bits = (sign << 31U) | (exponentValue << 23U) | ((normalized & 0x3ffU) << 13U);
        }
    } else if (exponent == 0x1fU) {
        bits = (sign << 31U) | 0x7f800000U | (mantissa << 13U);
    } else {
        bits = (sign << 31U) | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    float result{};
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

std::uint16_t NativeOidnProvider::floatToHalf(float value) noexcept {
    std::uint32_t bits{};
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

std::vector<float> NativeOidnProvider::decodeRgb(std::span<const std::uint8_t> bytes, std::uint32_t width,
                                                 std::uint32_t height) {
    const auto pixels = static_cast<std::size_t>(width) * height;
    if (pixels == 0)
        throw std::invalid_argument("OIDN texture extent is empty");
    const auto expected16 = pixels * 8U;
    const auto expected32 = pixels * 16U;
    const auto expected8 = pixels * 4U;
    std::vector<float> result(pixels * 3U);
    if (bytes.size() == expected16) {
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const auto offset = pixel * 8U;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto half = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(bytes[offset + channel * 2U]) |
                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + channel * 2U + 1U]) << 8U));
                result[pixel * 3U + channel] = halfToFloat(half);
            }
        }
        return result;
    }
    if (bytes.size() == expected32) {
        for (std::size_t pixel = 0; pixel < pixels; ++pixel)
            for (std::size_t channel = 0; channel < 3; ++channel) {
                float value{};
                std::memcpy(&value, bytes.data() + pixel * 16U + channel * sizeof(float), sizeof(value));
                result[pixel * 3U + channel] = value;
            }
        return result;
    }
    if (bytes.size() == expected8) {
        for (std::size_t pixel = 0; pixel < pixels; ++pixel)
            for (std::size_t channel = 0; channel < 3; ++channel)
                result[pixel * 3U + channel] = static_cast<float>(bytes[pixel * 4U + channel]) / 255.0F;
        return result;
    }
    throw std::invalid_argument("OIDN texture readback format is not RGBA8/RGBA16F/RGBA32F");
}

std::vector<std::uint8_t> NativeOidnProvider::encodeRgba16(std::span<const float> rgb, std::uint32_t width,
                                                           std::uint32_t height) {
    const auto pixels = static_cast<std::size_t>(width) * height;
    if (rgb.size() != pixels * 3U)
        throw std::invalid_argument("OIDN output sample count does not match the texture extent");
    std::vector<std::uint8_t> result(pixels * 8U);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto value = std::clamp(rgb[pixel * 3U + channel], 0.0F, std::numeric_limits<float>::max());
            const auto half = floatToHalf(value);
            result[pixel * 8U + channel * 2U] = static_cast<std::uint8_t>(half & 0xffU);
            result[pixel * 8U + channel * 2U + 1U] = static_cast<std::uint8_t>(half >> 8U);
        }
        result[pixel * 8U + 6U] = 0;
        result[pixel * 8U + 7U] = 0x3c; // alpha = 1.0 in binary16
    }
    return result;
}

bool NativeOidnProvider::execute(const fx::FxOidnDispatch& dispatch, const fx::FxFrameContext& context,
                                 CommandList& commands, const FxExecutionResources::TypedResourceResolver& resolve,
                                 std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr) {
        setError(error, "OIDN host has no device");
        return false;
    }
    if (!resolve) {
        setError(error, "OIDN host has no typed resource resolver");
        return false;
    }
    const auto findResource = [&resolve](std::string_view name, bool required) {
        if (name.empty()) {
            if (required)
                throw std::invalid_argument("OIDN resource name is empty");
            return FxExecutionResources::TypedResource{};
        }
        const auto binding = resolve(name);
        if (!binding.has_value() || !binding->valid())
            throw std::invalid_argument("OIDN resource is unavailable: " + std::string(name));
        return *binding;
    };
    try {
        if (context.renderWidth == 0 || context.renderHeight == 0)
            throw std::invalid_argument("OIDN render extent is empty");
        const auto width = static_cast<std::size_t>(context.renderWidth);
        const auto height = static_cast<std::size_t>(context.renderHeight);
        if (width > std::numeric_limits<std::size_t>::max() / height)
            throw std::overflow_error("OIDN render pixel count overflows host size");
        const auto pixelCount = width * height;
        if (pixelCount > std::numeric_limits<std::size_t>::max() / sizeof(NativeSceneOidnInput) ||
            pixelCount > std::numeric_limits<std::size_t>::max() / 3U)
            throw std::overflow_error("OIDN input size overflows host size");

        const auto input = findResource(dispatch.input, true);
        const bool inputIsTexture = input.texture.valid() && !input.buffer.valid() && !input.sampler.valid() &&
                                    !input.accelerationStructure.valid();
        const bool inputIsBuffer = input.buffer.valid() && !input.texture.valid() && !input.sampler.valid() &&
                                   !input.accelerationStructure.valid();
        if (!inputIsTexture && !inputIsBuffer)
            throw std::invalid_argument("OIDN input must resolve to exactly one texture or structured buffer");
        const auto outputBinding = findResource(dispatch.output, true);
        if (!outputBinding.texture.valid() || outputBinding.buffer.valid() || outputBinding.sampler.valid() ||
            outputBinding.accelerationStructure.valid())
            throw std::invalid_argument("OIDN output must resolve to a texture");
        const auto findOptionalTexture = [&findResource](std::string_view name) {
            const auto binding = findResource(name, false);
            if (name.empty())
                return handles::TextureHandle{};
            if (!binding.texture.valid() || binding.buffer.valid() || binding.sampler.valid() ||
                binding.accelerationStructure.valid())
                throw std::invalid_argument("OIDN auxiliary resource must resolve to a texture: " + std::string(name));
            return binding.texture;
        };
        const auto albedo = findOptionalTexture(dispatch.albedo);
        const auto normal = findOptionalTexture(dispatch.normal);
        const auto output = outputBinding.texture;
        const bool hasBufferInput = inputIsBuffer;
        const bool denoiserReady = denoiser_.ensure(context.renderWidth, context.renderHeight);
        if (!denoiserReady && !denoiser_.available() && !hasBufferInput) {
            commands.transferBarrierEx();
            if (input.texture != output)
                commands.copyTextureEx(input.texture, output);
            return true;
        }

        // The beauty pass is still being recorded at this point. Submit and
        // wait for that work before using the device's immediate readback and
        // upload context, then continue recording later FX passes.
        commands.flushAndWaitForHostReadbackEx();
        std::vector<float> beauty;
        std::vector<float> albedoValues;
        std::vector<float> normalValues;
        if (hasBufferInput) {
            const auto byteCount = pixelCount * sizeof(NativeSceneOidnInput);
            const auto inputBytes = device_->readbackBufferEx(input.buffer, 0, byteCount);
            if (inputBytes.size() != byteCount)
                throw std::runtime_error("OIDN structured buffer readback size does not match its extent");
            const auto sampleCount = pixelCount * 3U;
            beauty.resize(sampleCount);
            albedoValues.resize(sampleCount);
            normalValues.resize(sampleCount);
            for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
                NativeSceneOidnInput sample{};
                std::memcpy(&sample, inputBytes.data() + pixel * sizeof(sample), sizeof(sample));
                std::copy_n(sample.color, 3, beauty.begin() + static_cast<std::ptrdiff_t>(pixel * 3U));
                std::copy_n(sample.albedo, 3, albedoValues.begin() + static_cast<std::ptrdiff_t>(pixel * 3U));
                std::copy_n(sample.normal, 3, normalValues.begin() + static_cast<std::ptrdiff_t>(pixel * 3U));
            }
        } else {
            const auto beautyBytes = device_->readbackTextureEx(input.texture, 0, 0);
            beauty = decodeRgb(beautyBytes, context.renderWidth, context.renderHeight);
        }
        if (albedo.valid())
            albedoValues =
                decodeRgb(device_->readbackTextureEx(albedo, 0, 0), context.renderWidth, context.renderHeight);
        if (normal.valid())
            normalValues =
                decodeRgb(device_->readbackTextureEx(normal, 0, 0), context.renderWidth, context.renderHeight);
        std::vector<float> denoised(beauty.size());
        const core::DenoiserExecuteArgs args{.width = context.renderWidth,
                                             .height = context.renderHeight,
                                             .beauty = beauty,
                                             .albedo = albedoValues,
                                             .normal = normalValues,
                                             .output = denoised};
        if (!denoiser_.execute(args)) {
            setError(error, "OIDN denoiser execution failed");
            return false;
        }
        const auto bytes = encodeRgba16(denoised, context.renderWidth, context.renderHeight);
        device_->uploadTextureEx(output, bytes, 0, 0);
        commands.transferBarrierEx();
        return true;
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        return false;
    } catch (...) {
        setError(error, "OIDN host execution failed");
        return false;
    }
}

} // namespace dayo::graphics
